//! Optional global gaze source. All bus work runs on a dedicated thread.

#[path = "kwin_cursor/bus.rs"]
mod bus;

use std::ffi::{CStr, CString};
use std::fs::File;
use std::io::{self, Write};
use std::os::fd::AsRawFd;
use std::sync::{Arc, Mutex, mpsc};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

const STALE: Duration = Duration::from_secs(1);
const LOST: Duration = Duration::from_secs(3);
const RETRY: Duration = Duration::from_secs(2);
const OWNER_INTERVAL: Duration = Duration::from_millis(250);

pub(crate) struct KWinCursor {
    shared: Arc<Mutex<Target>>,
    stop: Option<mpsc::Sender<()>>,
    worker: Option<JoinHandle<()>>,
}

impl KWinCursor {
    /// Only the worker talks to KWin.
    pub(crate) fn start() -> io::Result<Self> {
        let (stop, receiver) = mpsc::channel();
        let shared = Arc::new(Mutex::new(Target::default()));
        let samples = Arc::clone(&shared);
        let worker = thread::Builder::new()
            .name("dw-kwin-cursor".into())
            .spawn(move || work(&samples, &receiver))?;
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

fn work(shared: &Mutex<Target>, stop: &mpsc::Receiver<()>) {
    while !stopping(stop, Duration::ZERO) {
        let _ = attempt(shared, stop);
        if stopping(stop, RETRY) {
            break;
        }
    }
}

fn attempt(shared: &Mutex<Target>, stop: &mpsc::Receiver<()>) -> io::Result<()> {
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
    if stopping(stop, Duration::ZERO) {
        return Ok(());
    }
    let result = (|| {
        let id = bus.load(&owner, &script_file, &script_name)?;
        if id < 0 {
            return Err(io::Error::other("KWin rejected cursor script"));
        }
        let script_object = CString::new(format!("/Scripting/Script{id}"))?;
        bus.run(&owner, &script_object)?;
        receive(&bus, &owner, shared, stop)
    })();
    shared.lock().unwrap().sample = None;
    // Use the same connection and KWin owner even if loading or running timed out.
    // If the connection is already gone, a running script cleans itself up.
    let _ = bus.unload(&owner, &script_name);
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
            if let Some(message) = message
                && message.is_sample()
                && receive_sample(message, owner, shared)
            {
                last_sample = Instant::now();
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
        if let Some(age) = age
            && let Some(observed) = sample.observed.checked_sub(age)
        {
            target.put(Sample { observed, ..sample }, &output, &generation);
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
