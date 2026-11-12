//! Opt-in proof that `deskwaifu-rust --overlay` really draws a Cubism model into
//! a layer-shell window through the Cubism Framework and EGL.
//!
//! `DESKWAIFU_RUNTIME_LIVE_TEST=1` opts in, following the `deskwaifu-overlay`
//! convention: without it each live test prints exactly one skip line on fd 2 and
//! returns, so the ordinary workspace run and CI stay green for headless
//! environments. With it set, every assertion below is mandatory and a missing
//! `kwin_wayland` fails loudly instead of skipping.
//!
//! The test also needs `CUBISM_SDK_ROOT` (external shaders) and
//! `CUBISM_TEST_MODEL` (a real external `Haru.model3.json`); see the README.

use serde_json::{json, Value};
use std::fs;
use std::io::{BufRead, BufReader, Read, Write};
use std::os::unix::fs::PermissionsExt;
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::Mutex;
use std::time::{Duration, Instant};

const LIVE_TEST_ENV: &str = "DESKWAIFU_RUNTIME_LIVE_TEST";
const TIMEOUT: Duration = Duration::from_secs(60);

/// Serializes the tests: both mutate the process-wide `XDG_RUNTIME_DIR` and
/// `WAYLAND_DISPLAY` variables and spawn a private compositor.
static COMPOSITOR_LOCK: Mutex<()> = Mutex::new(());

fn lock_compositor() -> std::sync::MutexGuard<'static, ()> {
    COMPOSITOR_LOCK
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn skip(test: &str) {
    // Straight to fd 2: libtest captures `eprintln!` of a passing test, and the
    // skip must stay visible in an ordinary run.
    let message = format!("skipped ({test}): set {LIVE_TEST_ENV}=1 to run the live overlay test\n");
    unsafe { libc_write(2, message.as_bytes()) };
}

/// `libc::write` without a libc dependency in this crate's test target.
unsafe fn libc_write(fd: i32, bytes: &[u8]) {
    extern "C" {
        fn write(fd: i32, buffer: *const std::ffi::c_void, count: usize) -> isize;
    }
    let _ = write(fd, bytes.as_ptr().cast(), bytes.len());
}

fn live_test_enabled(test: &str) -> bool {
    if std::env::var_os(LIVE_TEST_ENV).as_deref() != Some("1".as_ref()) {
        skip(test);
        return false;
    }
    let available = Command::new("kwin_wayland")
        .arg("--version")
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .is_ok_and(|status| status.success());
    assert!(
        available,
        "the `kwin_wayland` binary is required for {LIVE_TEST_ENV}=1 but was not found on PATH"
    );
    true
}

fn fixture() -> PathBuf {
    let path = std::env::var_os("CUBISM_TEST_MODEL").expect(
        "set CUBISM_TEST_MODEL to an external Haru.model3.json; the live test is not skipped",
    );
    fs::canonicalize(path).expect("canonicalize CUBISM_TEST_MODEL")
}

fn sdk_root() -> PathBuf {
    PathBuf::from(std::env::var_os("CUBISM_SDK_ROOT").expect("set CUBISM_SDK_ROOT"))
}

/// A private `kwin_wayland --virtual` instance, killed and reaped on drop.
struct Compositor {
    child: Child,
    runtime_dir: PathBuf,
    socket_name: String,
}

impl Compositor {
    fn spawn() -> Compositor {
        let runtime_dir = tempfile::Builder::new()
            .prefix("deskwaifu-runtime-")
            .tempdir()
            .expect("create private XDG_RUNTIME_DIR")
            .keep();
        fs::set_permissions(&runtime_dir, fs::Permissions::from_mode(0o700))
            .expect("chmod private XDG_RUNTIME_DIR");
        let socket_name = format!("deskwaifu-runtime-{}", std::process::id());
        let child = Command::new("kwin_wayland")
            .args([
                "--virtual",
                "--no-lockscreen",
                "--no-global-shortcuts",
                "--no-kactivities",
                "--width",
                "800",
                "--height",
                "600",
                "--socket",
                &socket_name,
            ])
            .env("XDG_RUNTIME_DIR", &runtime_dir)
            .env("WAYLAND_DISPLAY", &socket_name)
            .env_remove("WAYLAND_SOCKET")
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .expect("spawn the private compositor");
        let socket = runtime_dir.join(&socket_name);
        let deadline = Instant::now() + TIMEOUT;
        while Instant::now() < deadline {
            if socket.exists() {
                return Compositor {
                    child,
                    runtime_dir,
                    socket_name,
                };
            }
            std::thread::sleep(Duration::from_millis(20));
        }
        let mut child = child;
        let _ = child.kill();
        let _ = child.wait();
        panic!(
            "kwin_wayland did not create {} within {TIMEOUT:?}",
            socket.display()
        );
    }

    /// The private display the runtime under test must use.
    fn env(&self) -> Vec<(&'static str, String)> {
        vec![
            ("XDG_RUNTIME_DIR", self.runtime_dir.display().to_string()),
            ("WAYLAND_DISPLAY", self.socket_name.clone()),
        ]
    }
}

impl Drop for Compositor {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
        let _ = fs::remove_dir_all(&self.runtime_dir);
    }
}

struct Runtime {
    child: Child,
    socket: PathBuf,
    _dir: tempfile::TempDir,
}

impl Runtime {
    fn start(compositor: &Compositor, args: &[&std::ffi::OsStr]) -> Runtime {
        let dir = tempfile::tempdir().unwrap();
        let socket = dir.path().join("runtime.sock");
        let mut command = Command::new(env!("CARGO_BIN_EXE_deskwaifu-rust"));
        command
            .arg("--overlay")
            .args(args)
            .arg("--socket")
            .arg(&socket)
            .env("CUBISM_SDK_ROOT", sdk_root())
            .stderr(Stdio::piped());
        for (key, value) in compositor.env() {
            command.env(key, value);
        }
        let child = command.spawn().unwrap();
        let mut runtime = Runtime {
            child,
            socket,
            _dir: dir,
        };
        let deadline = Instant::now() + TIMEOUT;
        loop {
            if UnixStream::connect(&runtime.socket).is_ok() {
                return runtime;
            }
            if let Some(status) = runtime.child.try_wait().unwrap() {
                panic!("runtime exited ({status}): {}", runtime.stderr());
            }
            assert!(Instant::now() < deadline, "runtime did not bind in time");
            std::thread::sleep(Duration::from_millis(20));
        }
    }

    fn request(&self, request: Value) -> Value {
        let mut stream = UnixStream::connect(&self.socket).unwrap();
        stream.set_read_timeout(Some(TIMEOUT)).unwrap();
        stream.set_write_timeout(Some(TIMEOUT)).unwrap();
        writeln!(stream, "{request}").unwrap();
        let mut response = String::new();
        BufReader::new(stream).read_line(&mut response).unwrap();
        serde_json::from_str(&response).unwrap()
    }

    /// Polls `debug_frame` until the runtime reports a drawn, visible frame.
    /// `parameter` asks the reply to also report the rendered model's value for
    /// that parameter ID.
    fn settled_frame(&self, parameter: Option<&str>) -> Value {
        let request = match parameter {
            Some(id) => json!({"command":"debug_frame","args":{"id":id}}),
            None => json!({"command":"debug_frame"}),
        };
        let deadline = Instant::now() + TIMEOUT;
        let mut last = Value::Null;
        while Instant::now() < deadline {
            let reply = self.request(request.clone());
            if reply["ok"] == true
                && reply["frames_drawn"].as_u64().unwrap_or(0) > 0
                && reply["visible_pixels"].as_u64().unwrap_or(0) > 0
            {
                return reply;
            }
            last = reply;
            std::thread::sleep(Duration::from_millis(50));
        }
        panic!("no visible frame within {TIMEOUT:?}; last reply: {last}");
    }

    fn stderr(&mut self) -> String {
        let mut text = String::new();
        if let Some(mut stderr) = self.child.stderr.take() {
            let _ = stderr.read_to_string(&mut text);
        }
        text
    }

    fn quit(mut self) {
        assert_eq!(self.request(json!({"command":"quit"}))["ok"], true);
        let deadline = Instant::now() + TIMEOUT;
        while Instant::now() < deadline {
            if self.child.try_wait().unwrap().is_some() {
                return;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        panic!("runtime did not exit after quit");
    }
}

impl Drop for Runtime {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

/// One model directory holding a copy of the fixture, so a model switch is real.
fn model_dir(root: &Path, name: &str, fixture: &Path) -> PathBuf {
    let target = root.join(name);
    fs::create_dir_all(&target).unwrap();
    let source = fixture.parent().unwrap();
    copy_tree(source, &target);
    target
}

fn copy_tree(source: &Path, target: &Path) {
    for entry in fs::read_dir(source).unwrap().flatten() {
        let path = entry.path();
        let destination = target.join(entry.file_name());
        if path.is_dir() {
            fs::create_dir_all(&destination).unwrap();
            copy_tree(&path, &destination);
        } else {
            fs::copy(&path, &destination).unwrap();
        }
    }
}

fn count(reply: &Value, key: &str) -> u64 {
    reply[key]
        .as_u64()
        .unwrap_or_else(|| panic!("{key} in {reply}"))
}

#[test]
fn overlay_draws_a_real_model_and_reacts_to_parameters_and_model_switches() {
    if !live_test_enabled("overlay_draws_a_real_model_and_reacts_to_parameters_and_model_switches")
    {
        return;
    }
    let _compositor_lock = lock_compositor();
    let compositor = Compositor::spawn();
    let fixture = fixture();
    let dir = tempfile::tempdir().unwrap();
    model_dir(dir.path(), "Alpha", &fixture);
    model_dir(dir.path(), "Beta", &fixture);
    let runtime = Runtime::start(
        &compositor,
        &["--models-dir".as_ref(), dir.path().as_os_str()],
    );

    // 1. The status claims a renderer only after a frame was drawn.
    let status = runtime.request(json!({"command":"get_status"}));
    assert_eq!(status["ok"], true, "{status}");
    assert_eq!(status["renderer_available"], true, "{status}");
    assert_eq!(status["renderer_error"], Value::Null, "{status}");
    let drawn = count(&status, "frames_drawn");
    assert!(drawn > 0, "a frame must have been drawn: {status}");
    let visible = count(&status, "visible_pixels");
    let transparent = count(&status, "transparent_pixels");
    assert!(visible > 1000, "the model must cover real pixels: {status}");
    assert!(
        transparent > 1000,
        "the layer must stay transparent around the model: {status}"
    );
    assert_eq!(status["width"], 640);
    assert_eq!(status["height"], 480);
    eprintln!("live: drawn={drawn} visible={visible} transparent={transparent}");

    // 2. A parameter reaches the drawn frame.
    let parameters = runtime.request(json!({"command":"get_parameters"}));
    let angle = parameters["parameters"]
        .as_array()
        .unwrap()
        .iter()
        .find(|parameter| parameter["id"] == "ParamAngleX")
        .expect("Haru declares ParamAngleX");
    let index = angle["index"].as_u64().unwrap();
    let before = runtime.settled_frame(Some("ParamAngleX"));
    let value = angle["maximum"].as_f64().unwrap();
    let reply =
        runtime.request(json!({"command":"set_parameter","args":{"index":index,"value":value}}));
    assert_eq!(reply["ok"], true, "{reply}");
    assert_eq!(reply["value"], value);
    let after = runtime.settled_frame(Some("ParamAngleX"));
    assert_eq!(
        after["parameter_value"].as_f64().unwrap() as f32,
        value as f32,
        "the rendered model must carry the written value: {after}"
    );
    assert_ne!(
        count(&after, "pixel_checksum"),
        count(&before, "pixel_checksum"),
        "a parameter change must change the drawn pixels: before={before} after={after}"
    );
    eprintln!(
        "live: parameter changed the frame (visible {} -> {})",
        count(&before, "visible_pixels"),
        count(&after, "visible_pixels")
    );

    // 3. A model switch rebuilds the renderer and keeps drawing.
    let reply = runtime.request(json!({"command":"set_model","args":{"name":"Beta"}}));
    assert_eq!(reply["ok"], true, "{reply}");
    assert_eq!(reply["model"], "Beta");
    // The switch itself draws a frame, so the status is truthful at once.
    let immediate = runtime.request(json!({"command":"get_status"}));
    assert_eq!(immediate["renderer_available"], true, "{immediate}");
    assert_eq!(immediate["renderer_error"], Value::Null, "{immediate}");
    assert!(
        count(&immediate, "frames_drawn") > 0,
        "the switch must draw a frame: {immediate}"
    );
    let switched = runtime.settled_frame(None);
    assert_eq!(switched["ok"], true, "{switched}");
    assert!(count(&switched, "visible_pixels") > 1000, "{switched}");
    let status = runtime.request(json!({"command":"get_status"}));
    assert_eq!(status["model"], "Beta");
    assert_eq!(status["renderer_available"], true, "{status}");
    assert_eq!(status["renderer_error"], Value::Null, "{status}");
    eprintln!(
        "live: after switch model={} visible={}",
        status["model"],
        count(&status, "visible_pixels")
    );
    runtime.quit();
}

#[test]
fn overlay_without_a_compositor_fails_with_a_clear_error() {
    // Not a live test: no compositor is needed to prove the flag fails cleanly.
    let _compositor_lock = lock_compositor();
    let Some(model) = std::env::var_os("CUBISM_TEST_MODEL") else {
        skip("overlay_without_a_compositor_fails_with_a_clear_error (needs CUBISM_TEST_MODEL)");
        return;
    };
    let dir = tempfile::tempdir().unwrap();
    let socket = dir.path().join("runtime.sock");
    let mut child = Command::new(env!("CARGO_BIN_EXE_deskwaifu-rust"))
        .arg("--overlay")
        .arg("--model")
        .arg(model)
        .arg("--socket")
        .arg(&socket)
        .env_remove("WAYLAND_DISPLAY")
        .env_remove("WAYLAND_SOCKET")
        .stderr(Stdio::piped())
        .spawn()
        .unwrap();
    let deadline = Instant::now() + TIMEOUT;
    let status = loop {
        if let Some(status) = child.try_wait().unwrap() {
            break status;
        }
        assert!(
            Instant::now() < deadline,
            "the runtime must fail instead of running without a compositor"
        );
        std::thread::sleep(Duration::from_millis(20));
    };
    let mut stderr = String::new();
    child
        .stderr
        .take()
        .unwrap()
        .read_to_string(&mut stderr)
        .unwrap();
    assert!(!status.success(), "overlay mode must fail: {stderr}");
    assert!(
        stderr.contains("WAYLAND_DISPLAY"),
        "the error must name the missing display: {stderr}"
    );
    assert!(
        !socket.exists(),
        "a failed overlay start must not leave a socket behind"
    );
}
