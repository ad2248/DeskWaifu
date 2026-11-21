//! Optional global gaze source. All bus work runs on a dedicated thread.

#[path = "kwin_cursor/bus.rs"]
mod bus;

use std::ffi::{CStr, CString};
use std::fs::File;
use std::io::{self, Read, Write};
use std::os::fd::AsRawFd;
use std::os::unix::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use std::process::{ChildStdin, ChildStdout, Command, Stdio};
use std::sync::{Arc, Mutex, mpsc};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use rustix::event::{PollFd, PollFlags, Timespec, poll};

const STALE: Duration = Duration::from_secs(1);
const LOST: Duration = Duration::from_secs(3);
const RETRY: Duration = Duration::from_secs(2);
const OWNER_INTERVAL: Duration = Duration::from_millis(250);
const GUARD_NAME: &str = "deskwaifu-cursor-guard";

pub(crate) struct KWinCursor {
    shared: Arc<Mutex<Target>>,
    stop: Option<mpsc::Sender<()>>,
    worker: Option<JoinHandle<()>>,
}

impl KWinCursor {
    /// Starts the optional provider with the installed sibling cleanup helper.
    pub(crate) fn start() -> io::Result<Self> {
        Self::start_with_guard(&std::env::current_exe()?.with_file_name(GUARD_NAME))
    }

    /// Use an explicitly selected packaged helper. Only the worker talks to KWin.
    pub(crate) fn start_with_guard(guard: &Path) -> io::Result<Self> {
        let path = guard.to_owned();
        let (stop, receiver) = mpsc::channel();
        let shared = Arc::new(Mutex::new(Target::default()));
        let samples = Arc::clone(&shared);
        let worker = thread::Builder::new()
            .name("dw-kwin-cursor".into())
            .spawn(move || work(path, &samples, &receiver))?;
        Ok(Self {
            shared,
            stop: Some(stop),
            worker: Some(worker),
        })
    }

    /// Supply the current Wayland connector name, or an empty name for no output.
    pub(crate) fn set_output(&self, output: &str) {
        let mut target = self.shared.lock().unwrap();
        if target.output != output {
            target.output = output.to_owned();
            target.generation = target.generation.wrapping_add(1);
            target.sample = None;
        }
    }

    /// Maps to the overlay's logical size. Coordinates may be outside the overlay.
    pub(crate) fn local_position(&self, width: u32, height: u32) -> Option<(f64, f64)> {
        self.shared
            .lock()
            .unwrap()
            .fresh(Instant::now())?
            .local(width, height)
    }

    #[allow(dead_code)] // Callers can retain the local fallback independently of gaze.
    pub(crate) fn fullscreen(&self) -> Option<bool> {
        Some(
            self.shared
                .lock()
                .unwrap()
                .fresh(Instant::now())?
                .fullscreen,
        )
    }
}

impl Drop for KWinCursor {
    fn drop(&mut self) {
        drop(self.stop.take());
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

#[derive(Default)]
struct Target {
    output: String,
    generation: u64,
    sample: Option<Sample>,
}

impl Target {
    fn fresh(&self, now: Instant) -> Option<&Sample> {
        self.sample
            .as_ref()
            .filter(|sample| now.duration_since(sample.observed) < STALE)
    }

    fn put(&mut self, sample: Sample, output: &str, generation: &str) {
        if generation.parse::<u64>() == Ok(self.generation) {
            self.sample = (!self.output.is_empty()
                && self.output == output
                && sample.size.iter().all(|&dimension| dimension > 0.0))
            .then_some(sample);
        }
    }
}

struct Sample {
    observed: Instant,
    cursor: [f64; 2],
    origin: [f64; 2],
    size: [f64; 2],
    fullscreen: bool,
}

impl Sample {
    fn local(&self, width: u32, height: u32) -> Option<(f64, f64)> {
        if width == 0 || height == 0 || self.size.iter().any(|&d| d <= 0.0) {
            return None;
        }
        let x = (self.cursor[0] - self.origin[0]) * f64::from(width) / self.size[0];
        let y = (self.cursor[1] - self.origin[1]) * f64::from(height) / self.size[1];
        (x.is_finite() && y.is_finite()).then_some((x, y))
    }
}

fn stopping(stop: &mpsc::Receiver<()>, wait: Duration) -> bool {
    stop.recv_timeout(wait) != Err(mpsc::RecvTimeoutError::Timeout)
}

fn work(path: PathBuf, shared: &Mutex<Target>, stop: &mpsc::Receiver<()>) {
    while !stopping(stop, Duration::ZERO) {
        let _ = attempt(&path, shared, stop);
        if stopping(stop, RETRY) {
            break;
        }
    }
}

fn attempt(helper: &Path, shared: &Mutex<Target>, stop: &mpsc::Receiver<()>) -> io::Result<()> {
    let bus = bus::Bus::open()?;
    let owner = bus.owner()?;
    let client = bus.own_name()?.to_owned();
    let mut script_name = format!("deskwaifu-cursor-{}-", std::process::id()).into_bytes();
    script_name.extend(client.to_bytes());
    let script_name = CString::new(script_name)?;
    let mut script = File::from(rustix::fs::memfd_create(
        "deskwaifu-kwin-cursor",
        rustix::fs::MemfdFlags::CLOEXEC,
    )?);
    // Both identifiers are unique bus names, which contain no JS string delimiters.
    let source = include_str!("kwin_cursor/sample.js")
        .replace("@DESTINATION@", client.to_str().map_err(io::Error::other)?)
        .replace("@OWNER@", owner.to_str().map_err(io::Error::other)?)
        .replace(
            "@SCRIPT_NAME@",
            script_name.to_str().map_err(io::Error::other)?,
        );
    script.write_all(source.as_bytes())?;
    let script_file = CString::new(format!(
        "/proc/{}/fd/{}",
        std::process::id(),
        script.as_raw_fd()
    ))?;
    let mut guard = Guard::start(helper, &owner, &client, &script_name)?;
    let result = (|| {
        if stopping(stop, Duration::ZERO) {
            return Ok(());
        }
        let id = bus.load(&owner, &script_file, &script_name)?;
        if id < 0 {
            return Err(io::Error::other("KWin rejected cursor script"));
        }
        let script_object = CString::new(format!("/Scripting/Script{id}"))?;
        bus.run(&owner, &script_object)?;
        receive(&bus, &owner, shared, stop)
    })();
    shared.lock().unwrap().sample = None;
    // Release the unique name before waking the guard, even when load timed out.
    drop(bus);
    guard.control.take();
    // While running, avoid registering another script until cleanup of this one
    // finishes. Host shutdown waits at most one poll interval, then the detached
    // guard can finish independently.
    while !guard.finished() && !stopping(stop, Duration::ZERO) {}
    result
}

fn receive(
    bus: &bus::Bus,
    owner: &CStr,
    shared: &Mutex<Target>,
    stop: &mpsc::Receiver<()>,
) -> io::Result<()> {
    let mut last_sample = Instant::now();
    let mut owner_check = last_sample;
    while !stopping(stop, Duration::ZERO) {
        if owner_check.elapsed() >= OWNER_INTERVAL {
            if bus.owner()?.as_c_str() != owner {
                return Err(io::Error::other("KWin owner changed"));
            }
            owner_check = Instant::now();
        }
        for _ in 0..32 {
            let (progress, message) = bus.process()?;
            if let Some(message) = message {
                if message.is_sample() && receive_sample(message, owner, shared) {
                    last_sample = Instant::now();
                }
            }
            if !progress {
                break;
            }
        }
        if last_sample.elapsed() >= LOST {
            return Err(io::Error::other("KWin cursor script timed out"));
        }
        bus.wait()?;
    }
    Ok(())
}

fn receive_sample(mut message: bus::Message, owner: &CStr, shared: &Mutex<Target>) -> bool {
    if !message.sender_is(owner) {
        let _ = message.reject(
            c"org.freedesktop.DBus.Error.AccessDenied",
            c"Only KWin may send samples",
        );
        return false;
    }
    let sample = (|| {
        if !message.has_sample_signature() {
            return None;
        }
        let mut field = || message.read_string().ok()?.into_string().ok();
        let x = number(&field()?)?;
        let y = number(&field()?)?;
        let output = field()?;
        let left = number(&field()?)?;
        let top = number(&field()?)?;
        let width = number(&field()?)?;
        let height = number(&field()?)?;
        let metadata = field()?;
        let mut metadata = metadata.split('|');
        let fullscreen = match metadata.next()? {
            "true" => true,
            "false" => false,
            _ => return None,
        };
        let generation = metadata.next()?.to_owned();
        let timestamp = metadata.next()?.parse::<u64>().ok()?;
        if metadata.next().is_some() {
            return None;
        }
        if width < 0.0 || height < 0.0 {
            return None;
        }
        Some((
            output,
            generation,
            timestamp,
            Sample {
                observed: Instant::now(),
                cursor: [x, y],
                origin: [left, top],
                size: [width, height],
                fullscreen,
            },
        ))
    })();
    let Some((output, generation, timestamp, sample)) = sample else {
        let _ = message.reject(
            c"org.freedesktop.DBus.Error.InvalidArgs",
            c"Invalid cursor sample",
        );
        return false;
    };
    let age = sample_age(SystemTime::now(), timestamp);
    let target_reply = {
        let mut target = shared.lock().unwrap();
        if let Some(age) = age {
            if let Some(observed) = sample.observed.checked_sub(age) {
                target.put(Sample { observed, ..sample }, &output, &generation);
            }
        }
        CString::new(format!("{}\n{}", target.generation, target.output))
    };
    // A stale queued callback still gets a reply so the script can sample again.
    if let Ok(reply) = target_reply {
        let _ = message.reply(&reply);
    } else {
        let _ = message.reply(c"\n");
    }
    age.is_some()
}

fn number(s: &str) -> Option<f64> {
    (s.len() <= 64).then_some(())?;
    s.parse::<f64>().ok().filter(|value| value.is_finite())
}

fn sample_age(now: SystemTime, timestamp: u64) -> Option<Duration> {
    let sent = UNIX_EPOCH.checked_add(Duration::from_millis(timestamp))?;
    let age = now.duration_since(sent).ok()?;
    (age < STALE).then_some(age)
}

struct Guard {
    control: Option<ChildStdin>,
    status: ChildStdout,
}

impl Guard {
    fn start(path: &Path, owner: &CStr, client: &CStr, script: &CStr) -> io::Result<Self> {
        let mut launcher = Command::new(path)
            .arg("launch")
            .args(
                [owner, client, script].map(|value| std::ffi::OsStr::from_bytes(value.to_bytes())),
            )
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()?;
        let mut guard = Self {
            control: launcher.stdin.take(),
            status: launcher.stdout.take().unwrap(),
        };
        let mut ack = [0];
        let ready = poll_pipe(&guard.status, 1_000)
            && guard.status.read_exact(&mut ack).is_ok()
            && ack == *b"R";
        if !ready {
            let _ = launcher.kill();
        }
        let success = launcher.wait()?.success();
        if !ready || !success {
            return Err(io::Error::other("KWin cleanup guard is not ready"));
        }
        Ok(guard)
    }

    fn finished(&self) -> bool {
        poll_pipe(&self.status, 250)
    }
}

fn poll_pipe(pipe: &ChildStdout, millis: i64) -> bool {
    let mut descriptors = [PollFd::new(pipe, PollFlags::IN)];
    let timeout = Timespec {
        tv_sec: millis / 1_000,
        tv_nsec: (millis % 1_000) * 1_000_000,
    };
    poll(&mut descriptors, Some(&timeout)).is_ok_and(|ready| ready > 0)
}
