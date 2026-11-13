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

/// Hand-written bindings for KWin's `org_kde_kwin_fake_input` (version 3).
///
/// `rust/overlay/tests/kwin.rs` generates the same protocol with
/// `wayland-scanner`; this test target has no such dev-dependency, so the wire
/// descriptors are declared by hand the way `cubism-core/src/ffi.rs` declares the
/// Cubism Core C API. The request order below *is* the wire opcode order, and the
/// version-3 subset is the same one `rust/overlay/tests/fake-input.xml` proves.
mod fake_input {
    use std::ffi::CString;
    use std::os::fd::{AsRawFd, RawFd};
    use std::sync::Arc;

    use std::sync::OnceLock;

    use wayland_client::backend::protocol::{
        wl_interface, wl_message, Argument, ArgumentType, Interface, Message, MessageDesc,
    };
    use wayland_client::backend::{smallvec, Backend, ObjectData, ObjectId};
    use wayland_client::globals::{registry_queue_init, GlobalList, GlobalListContents};
    use wayland_client::protocol::wl_registry;
    use wayland_client::{Connection, Dispatch, EventQueue, Proxy, QueueHandle};

    const STR_STR: &[ArgumentType] = &[
        ArgumentType::Str(wayland_client::backend::protocol::AllowNull::No),
        ArgumentType::Str(wayland_client::backend::protocol::AllowNull::No),
    ];
    const FIXED_FIXED: &[ArgumentType] = &[ArgumentType::Fixed, ArgumentType::Fixed];
    const UINT_UINT: &[ArgumentType] = &[ArgumentType::Uint, ArgumentType::Uint];
    const UINT_FIXED: &[ArgumentType] = &[ArgumentType::Uint, ArgumentType::Fixed];
    const UINT_FIXED_FIXED: &[ArgumentType] =
        &[ArgumentType::Uint, ArgumentType::Fixed, ArgumentType::Fixed];
    const UINT: &[ArgumentType] = &[ArgumentType::Uint];
    const NONE: &[ArgumentType] = &[];

    const fn request(
        name: &'static str,
        signature: &'static [ArgumentType],
        since: u32,
    ) -> MessageDesc {
        MessageDesc {
            name,
            signature,
            since,
            is_destructor: false,
            child_interface: None,
            arg_interfaces: &[],
        }
    }

    /// Opcodes in declaration order, which is also the wire order.
    const REQUESTS: &[MessageDesc] = &[
        request("authenticate", STR_STR, 1),
        request("pointer_motion", FIXED_FIXED, 1),
        request("button", UINT_UINT, 1),
        request("axis", UINT_FIXED, 1),
        request("touch_down", UINT_FIXED_FIXED, 2),
        request("touch_motion", UINT_FIXED_FIXED, 2),
        request("touch_up", UINT, 2),
        request("touch_cancel", NONE, 2),
        request("touch_frame", NONE, 2),
        request("pointer_motion_absolute", FIXED_FIXED, 3),
    ];

    /// The interface name and the wire signatures of [`REQUESTS`], in the same
    /// order. The libwayland backend (`wayland-backend`'s `sys` mode, which the
    /// overlay needs for `libwayland-egl`) refuses an [`Interface`] without a C
    /// `wl_interface`, so both descriptions are built together and leaked.
    const NAMES: [&str; 10] = [
        "authenticate",
        "pointer_motion",
        "button",
        "axis",
        "touch_down",
        "touch_motion",
        "touch_up",
        "touch_cancel",
        "touch_frame",
        "pointer_motion_absolute",
    ];
    const SIGNATURES: [&str; 10] = ["ss", "ff", "uu", "uf", "uff", "uff", "u", "", "", "ff"];

    /// The one leaked `Interface`, built on first use.
    fn interface() -> &'static Interface {
        static ONCE: OnceLock<&'static Interface> = OnceLock::new();
        ONCE.get_or_init(|| {
            let messages: Vec<wl_message> = NAMES
                .iter()
                .zip(SIGNATURES)
                .map(|(name, signature)| wl_message {
                    // `into_raw` leaks the CString, which is what a static
                    // `wl_message` requires: these live for the whole process.
                    name: CString::new(*name).expect("static name").into_raw(),
                    signature: CString::new(signature)
                        .expect("static signature")
                        .into_raw(),
                    // No request has an object argument, so there is no types array.
                    types: std::ptr::null(),
                })
                .collect();
            let messages: &'static [wl_message] = Box::leak(messages.into_boxed_slice());
            let c = wl_interface {
                name: CString::new("org_kde_kwin_fake_input")
                    .expect("static name")
                    .into_raw(),
                version: 3,
                request_count: messages.len() as i32,
                requests: messages.as_ptr(),
                event_count: 0,
                events: std::ptr::null(),
            };
            let c: &'static wl_interface = Box::leak(Box::new(c));
            Box::leak(Box::new(Interface {
                name: "org_kde_kwin_fake_input",
                version: 3,
                requests: REQUESTS,
                events: &[],
                c_ptr: Some(c),
            }))
        })
    }

    const AUTHENTICATE: u16 = 0;
    const BUTTON: u16 = 2;
    const POINTER_MOTION_ABSOLUTE: u16 = 9;
    /// `wl_registry.bind`'s opcode.
    const REGISTRY_BIND: u16 = 0;

    /// `ObjectData` for an object that has no events to dispatch.
    #[derive(Debug)]
    struct NoEvents;

    impl ObjectData for NoEvents {
        fn event(
            self: Arc<Self>,
            _: &Backend,
            _: Message<ObjectId, std::os::fd::OwnedFd>,
        ) -> Option<Arc<dyn ObjectData>> {
            None
        }

        fn destroyed(&self, _: ObjectId) {}
    }

    /// The `wl_registry` state `registry_queue_init` needs.
    #[derive(Debug)]
    pub struct RegistryState;

    impl Dispatch<wl_registry::WlRegistry, GlobalListContents> for RegistryState {
        fn event(
            _: &mut Self,
            _: &wl_registry::WlRegistry,
            _: wl_registry::Event,
            _: &GlobalListContents,
            _: &Connection,
            _: &QueueHandle<Self>,
        ) {
        }
    }

    /// An authenticated `org_kde_kwin_fake_input` object on its own connection.
    pub struct FakeInput {
        connection: Connection,
        /// Held so the registry's own queue keeps dispatching until drop.
        _queue: EventQueue<RegistryState>,
        registry_state: RegistryState,
        object: ObjectId,
    }

    impl FakeInput {
        /// Binds the global and authenticates, so KWin accepts injected input.
        pub fn new(connection: Connection) -> FakeInput {
            let (globals, queue) =
                registry_queue_init::<RegistryState>(&connection).expect("registry for fake input");
            let global = global_name(&globals);
            let backend = connection.backend();
            let object = backend
                .send_request(
                    Message {
                        sender_id: Proxy::id(globals.registry()),
                        opcode: REGISTRY_BIND,
                        args: smallvec::smallvec![
                            Argument::Uint(global),
                            Argument::Str(Some(Box::new(
                                CString::new(interface().name).expect("static interface name")
                            ))),
                            Argument::Uint(interface().version),
                            Argument::NewId(ObjectId::null()),
                        ],
                    },
                    Some(Arc::new(NoEvents)),
                    Some((interface(), interface().version)),
                )
                .expect("bind org_kde_kwin_fake_input");
            let mut fake = FakeInput {
                connection,
                _queue: queue,
                registry_state: RegistryState,
                object,
            };
            fake.send(
                AUTHENTICATE,
                smallvec::smallvec![
                    Argument::Str(Some(Box::new(
                        CString::new("DeskWaifu runtime test").expect("static name")
                    ))),
                    Argument::Str(Some(Box::new(
                        CString::new("private overlay pointer-policy proof").expect("static text")
                    ))),
                ],
            );
            fake
        }

        fn send(&mut self, opcode: u16, args: smallvec::SmallVec<[Argument<ObjectId, RawFd>; 4]>) {
            let backend = self.connection.backend();
            backend
                .send_request(
                    Message {
                        sender_id: self.object.clone(),
                        opcode,
                        args,
                    },
                    None,
                    None,
                )
                .expect("inject fake input");
            // Flushing and a round-trip is the completion barrier: the compositor
            // has really processed the request before the caller reads runtime
            // state, which is the recipe `rust/overlay/tests/kwin.rs` proves.
            self.connection.flush().expect("flush fake input");
            self._queue
                .roundtrip(&mut self.registry_state)
                .expect("fake input barrier");
        }

        /// Moves the fake pointer to a compositor-global position.
        pub fn move_absolute(&mut self, x: i32, y: i32) {
            self.send(
                POINTER_MOTION_ABSOLUTE,
                smallvec::smallvec![Argument::Fixed(x * 256), Argument::Fixed(y * 256)],
            );
        }

        /// Injects a press or release of one Linux button code.
        pub fn button(&mut self, button: u32, pressed: bool) {
            self.send(
                BUTTON,
                smallvec::smallvec![Argument::Uint(button), Argument::Uint(u32::from(pressed)),],
            );
        }

        /// Moves to `(x, y)` and clicks `button`.
        pub fn click(&mut self, button: u32, x: i32, y: i32) {
            self.move_absolute(x, y);
            self.button(button, true);
            self.button(button, false);
        }
    }

    fn global_name(globals: &GlobalList) -> u32 {
        globals
            .contents()
            .with_list(|list| {
                list.iter()
                    .find(|global| global.interface == interface().name)
                    .map(|global| global.name)
            })
            .expect("KWin must advertise org_kde_kwin_fake_input v3")
    }

    // `AS_raw_fd` is only referenced by the backend's `RawFd` message type.
    const _: fn(&std::os::unix::net::UnixStream) -> RawFd = AsRawFd::as_raw_fd;
}

const LIVE_TEST_ENV: &str = "DESKWAIFU_RUNTIME_LIVE_TEST";
const TIMEOUT: Duration = Duration::from_secs(60);
/// The Linux `BTN_LEFT`/`BTN_RIGHT` codes the overlay maps to buttons 0 and 1.
const BTN_LEFT: u32 = 0x110;
const BTN_RIGHT: u32 = 0x111;

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

    /// A connection to this private compositor, used by the fake-input device.
    fn connect(&self) -> wayland_client::Connection {
        let socket =
            std::os::unix::net::UnixStream::connect(self.runtime_dir.join(&self.socket_name))
                .expect("connect to the private compositor");
        wayland_client::Connection::from_socket(socket).expect("wrap the private compositor socket")
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
        self.settled_frame_where(parameter, |reply| {
            reply["visible_pixels"].as_u64().unwrap_or(0) > 0
        })
    }

    /// Polls `debug_frame` until `ready` accepts the reply, so a test can wait
    /// for a state that is deliberately not "a visible model" (for example a
    /// hidden frame's empty input region).
    fn settled_frame_where(
        &self,
        parameter: Option<&str>,
        ready: impl Fn(&Value) -> bool,
    ) -> Value {
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
                && ready(&reply)
            {
                return reply;
            }
            last = reply;
            std::thread::sleep(Duration::from_millis(50));
        }
        panic!("no matching frame within {TIMEOUT:?}; last reply: {last}");
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

/// Every rectangle of a `debug_frame`/`get_status` reply must be a positive
/// area inside the surface, and no two may overlap: they are unioned into a
/// `wl_region`, and an accidental overlap would mean the clipped runs were
/// re-clipped or duplicated.
fn assert_region_inside_surface(reply: &Value) {
    let width = count(reply, "width") as i32;
    let height = count(reply, "height") as i32;
    let rects = region_rects(reply);
    assert_eq!(rects.len() as u64, count(reply, "input_regions"), "{reply}");
    for &(x, y, w, h) in &rects {
        assert!(
            w > 0 && h > 0 && x >= 0 && y >= 0 && x + w <= width && y + h <= height,
            "rect {x},{y} {w}x{h} is outside the {width}x{height} surface: {reply}"
        );
    }
    for (index, a) in rects.iter().enumerate() {
        for b in &rects[index + 1..] {
            let overlap = a.0.max(b.0) < (a.0 + a.2).min(b.0 + b.2)
                && a.1.max(b.1) < (a.1 + a.3).min(b.1 + b.3);
            assert!(!overlap, "{a:?} overlaps {b:?}: {reply}");
        }
    }
}

/// The `input_region` rectangle list of a reply, as `(x, y, width, height)`.
fn region_rects(reply: &Value) -> Vec<(i32, i32, i32, i32)> {
    reply["input_region"]
        .as_array()
        .unwrap_or_else(|| panic!("input_region in {reply}"))
        .iter()
        .map(|rect| {
            (
                rect["x"].as_i64().unwrap() as i32,
                rect["y"].as_i64().unwrap() as i32,
                rect["width"].as_i64().unwrap() as i32,
                rect["height"].as_i64().unwrap() as i32,
            )
        })
        .collect()
}

/// One model directory holding a copy of the fixture, so a model switch is real.
fn model_dir(root: &Path, name: &str, fixture: &Path) -> PathBuf {
    let target = root.join(name);
    fs::create_dir_all(&target).unwrap();
    let source = fixture.parent().unwrap();
    copy_tree(source, &target);
    target
}

/// A copy of the fixture whose `.model3.json` declares no `HitAreas`.
///
/// This is the model shape the C++ `LAppModel::HitTest` fallback exists for
/// ("models such as Vivianne that configure no HitAreas"), so a live test can
/// prove the documented fallback on the same real moc, textures and motions.
fn model_dir_without_hit_areas(root: &Path, name: &str, fixture: &Path) -> PathBuf {
    let target = model_dir(root, name, fixture);
    let json_path = target.join(fixture.file_name().unwrap());
    let mut document: Value =
        serde_json::from_str(&fs::read_to_string(&json_path).unwrap()).unwrap();
    document.as_object_mut().unwrap().remove("HitAreas");
    fs::write(&json_path, serde_json::to_vec(&document).unwrap()).unwrap();
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

    // The drawn model must produce a shaped input region, not a fully
    // click-through surface. Its rectangles come from the same readback the
    // pixel counts describe.
    let frame = runtime.settled_frame(None);
    let regions = count(&frame, "input_regions");
    assert!(
        regions > 0,
        "a drawn model must receive input somewhere: {frame}"
    );
    assert_eq!(frame["input_region_applied"], true, "{frame}");
    let region_pixels = count(&frame, "input_region_pixels");
    assert!(
        region_pixels > 0 && region_pixels <= 640 * 480,
        "the region covers {region_pixels} pixels of the surface: {frame}"
    );
    // Each covered 10x10 block is opaque only because its sampled centre pixel
    // is visible, so the region can never cover more than 100x the visible count.
    let frame_visible = count(&frame, "visible_pixels");
    assert!(
        region_pixels <= frame_visible * 100,
        "the region covers {region_pixels} pixels for only {frame_visible} visible ones: {frame}"
    );
    assert_region_inside_surface(&frame);
    eprintln!(
        "live: input_regions={regions} input_region_pixels={region_pixels} visible={visible}"
    );

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

/// A failed texture load must replace the compositor's old buffer and input
/// region, not merely drop the Rust renderer while leaving the model clickable.
#[test]
fn overlay_failed_reload_clears_pixels_and_input_region() {
    if !live_test_enabled("overlay_failed_reload_clears_pixels_and_input_region") {
        return;
    }
    let _compositor_lock = lock_compositor();
    let compositor = Compositor::spawn();
    let fixture = fixture();
    let dir = tempfile::tempdir().unwrap();
    model_dir(dir.path(), "Alpha", &fixture);
    let beta = model_dir(dir.path(), "Beta", &fixture);
    let runtime = Runtime::start(
        &compositor,
        &["--models-dir".as_ref(), dir.path().as_os_str()],
    );
    let drawn = runtime.settled_frame(None);
    assert!(count(&drawn, "input_regions") > 0, "{drawn}");
    let inside = largest_rect_centre(&drawn);
    // Discovery and Core loading still succeed; only Scene::load fails.
    let document: Value = serde_json::from_slice(&fs::read(&fixture).unwrap()).unwrap();
    let texture = document["FileReferences"]["Textures"][0].as_str().unwrap();
    fs::remove_file(beta.join(texture)).unwrap();
    let failed = runtime.request(json!({"command":"set_model","args":{"name":"Beta"}}));
    assert_eq!(failed["ok"], false, "{failed}");
    let cleared = runtime.request(json!({"command":"get_status"}));
    assert_eq!(cleared["renderer_available"], false, "{cleared}");
    assert!(
        cleared["renderer_error"]
            .as_str()
            .unwrap()
            .contains("cannot read texture"),
        "{cleared}"
    );
    assert_eq!(count(&cleared, "frames_drawn"), 0, "{cleared}");
    assert_eq!(count(&cleared, "visible_pixels"), 0, "{cleared}");
    assert_eq!(
        count(&cleared, "transparent_pixels"),
        640 * 480,
        "{cleared}"
    );
    assert_eq!(count(&cleared, "input_regions"), 0, "{cleared}");
    assert_eq!(count(&cleared, "input_region_pixels"), 0, "{cleared}");
    assert_eq!(cleared["input_region_applied"], true, "{cleared}");
    // Prove the compositor stopped routing clicks at a formerly opaque point.
    let mut fake = fake_input::FakeInput::new(compositor.connect());
    fake.move_absolute(1, 1);
    fake.click(BTN_LEFT, inside.0, inside.1);
    std::thread::sleep(Duration::from_millis(700));
    let after_click = runtime.request(json!({"command":"get_status"}));
    assert_eq!(
        count(&after_click, "taps"),
        count(&cleared, "taps"),
        "{after_click}"
    );
    assert_eq!(
        count(&after_click, "pointer_events"),
        count(&cleared, "pointer_events"),
        "{after_click}"
    );
    let recovered = runtime.request(json!({"command":"set_model","args":{"name":"Alpha"}}));
    assert_eq!(recovered["ok"], true, "{recovered}");
    let restored = runtime.settled_frame(None);
    assert!(count(&restored, "input_regions") > 0, "{restored}");
    runtime.quit();
}

/// An empty texture name reserves its model3 slot. Haru uses slots 0 and 1:
/// inserting a hole at 1 moves the later texture to unused slot 2. Binding must
/// fail for slot 2, not silently compact it back into slot 1 and draw with the
/// wrong declaration.
#[test]
fn overlay_empty_texture_slot_preserves_later_bindings() {
    if !live_test_enabled("overlay_empty_texture_slot_preserves_later_bindings") {
        return;
    }
    let _compositor_lock = lock_compositor();
    let compositor = Compositor::spawn();
    let fixture = fixture();
    let dir = tempfile::tempdir().unwrap();
    for name in ["Baseline", "Sparse"] {
        let target = model_dir(dir.path(), name, &fixture);
        let path = target.join(fixture.file_name().unwrap());
        let mut document: Value = serde_json::from_slice(&fs::read(&path).unwrap()).unwrap();
        assert!(
            document["FileReferences"]["Textures"]
                .as_array()
                .unwrap()
                .len()
                >= 2
        );
        if name == "Sparse" {
            document["FileReferences"]["Textures"]
                .as_array_mut()
                .unwrap()
                .insert(1, json!(""));
        }
        fs::write(path, serde_json::to_vec(&document).unwrap()).unwrap();
    }
    let runtime = Runtime::start(
        &compositor,
        &["--models-dir".as_ref(), dir.path().as_os_str()],
    );
    runtime.settled_frame(None);
    let sparse = runtime.request(json!({"command":"set_model","args":{"name":"Sparse"}}));
    assert_eq!(sparse["ok"], false, "{sparse}");
    assert!(
        sparse["error"]
            .as_str()
            .unwrap()
            .contains("cannot bind texture 2: texture slot not used by model"),
        "the later texture must bind at slot 2, not compact into slot 1: {sparse}"
    );
    let status = runtime.request(json!({"command":"get_status"}));
    assert_eq!(status["renderer_available"], false, "{status}");
    let restored = runtime.request(json!({"command":"set_model","args":{"name":"Baseline"}}));
    assert_eq!(restored["ok"], true, "{restored}");
    runtime.settled_frame(None);
    runtime.quit();
}

/// The degenerate ends of the alpha → region mapping, on a real compositor:
/// a drawn model yields a nonempty region inside the surface, a fully
/// transparent frame (the existing `toggle_hidden` command, which the draw path
/// honours by clearing without drawing) yields an empty one, and showing the
/// model again restores it.
#[test]
fn overlay_input_region_follows_the_rendered_alpha() {
    if !live_test_enabled("overlay_input_region_follows_the_rendered_alpha") {
        return;
    }
    let _compositor_lock = lock_compositor();
    let compositor = Compositor::spawn();
    let fixture = fixture();
    let runtime = Runtime::start(&compositor, &["--model".as_ref(), fixture.as_os_str()]);

    // 1. A real Haru frame: the region is nonempty and lies inside the surface.
    let drawn = runtime.settled_frame(None);
    let regions = count(&drawn, "input_regions");
    assert!(regions > 0, "the drawn model must be clickable: {drawn}");
    assert_eq!(drawn["input_region_applied"], true, "{drawn}");
    assert_region_inside_surface(&drawn);
    let visible = count(&drawn, "visible_pixels");
    let region_pixels = count(&drawn, "input_region_pixels");
    // The model is fitted at 90% of the surface, so a *shaped* region cannot
    // reach the surface corners: proving they are excluded proves the region is
    // derived from the alpha instead of being "the whole surface".
    let rects = region_rects(&drawn);
    for corner in [(0, 0), (639, 0), (0, 479), (639, 479)] {
        assert!(
            !rects.iter().any(|&(x, y, w, h)| {
                corner.0 >= x && corner.0 < x + w && corner.1 >= y && corner.1 < y + h
            }),
            "corner {corner:?} must stay click-through: {drawn}"
        );
    }
    assert!(
        region_pixels < 640 * 480,
        "a fitted model must not cover the whole surface: {drawn}"
    );
    eprintln!(
        "live: drawn input_regions={regions} input_region_pixels={region_pixels} visible={visible}"
    );

    // 2. Hide the model: the draw path clears to alpha 0, so the region must
    //    become empty and the status must say so.
    let hidden = runtime.request(json!({"command":"toggle_hidden"}));
    assert_eq!(hidden["ok"], true, "{hidden}");
    assert_eq!(hidden["hidden"], true, "{hidden}");
    let blank =
        runtime.settled_frame_where(None, |reply| reply["visible_pixels"].as_u64() == Some(0));
    assert_eq!(count(&blank, "input_regions"), 0, "{blank}");
    assert_eq!(count(&blank, "input_region_pixels"), 0, "{blank}");
    assert_eq!(blank["input_region_applied"], true, "{blank}");
    assert_eq!(
        blank["input_region"].as_array().map(Vec::len),
        Some(0),
        "{blank}"
    );
    let status = runtime.request(json!({"command":"get_status"}));
    assert_eq!(status["hidden"], true, "{status}");
    assert_eq!(count(&status, "input_regions"), 0, "{status}");
    assert_eq!(status["input_region_applied"], true, "{status}");
    eprintln!(
        "live: hidden input_regions={} visible={}",
        count(&status, "input_regions"),
        count(&status, "visible_pixels")
    );

    // 3. Show it again: the region comes back, so the empty region really
    //    tracked the frame and was not a one-way failure.
    let shown = runtime.request(json!({"command":"toggle_hidden"}));
    assert_eq!(shown["ok"], true, "{shown}");
    assert_eq!(shown["hidden"], false, "{shown}");
    let restored = runtime.settled_frame(None);
    let restored_regions = count(&restored, "input_regions");
    assert!(
        restored_regions > 0,
        "showing the model must restore its input region: {restored}"
    );
    assert_eq!(restored["input_region_applied"], true, "{restored}");
    assert_region_inside_surface(&restored);
    eprintln!(
        "live: restored input_regions={restored_regions} input_region_pixels={}",
        count(&restored, "input_region_pixels")
    );
    runtime.quit();
}

/// The C++ pointer policy on a real compositor, driven by injected input.
///
/// Every assertion is about the runtime's own counters, so nothing is inferred
/// from pixels: a click in the opaque region must become a `tap`, a click in a
/// transparent corner must not, a right click must switch the model, and a hidden
/// model must ignore clicks. The injected input is real
/// `org_kde_kwin_fake_input` traffic, the same recipe
/// `rust/overlay/tests/kwin.rs` proves.
#[test]
fn overlay_pointer_policy_reacts_to_real_clicks() {
    if !live_test_enabled("overlay_pointer_policy_reacts_to_real_clicks") {
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
    // The private compositor only materialises its fake pointer device once it is
    // used, and the seat advertises the capability after that.
    let mut fake = fake_input::FakeInput::new(compositor.connect());
    fake.move_absolute(1, 1);

    let before = runtime.request(json!({"command":"get_status"}));
    assert_eq!(before["taps"], 0, "{before}");
    assert_eq!(before["pointer_events"], 0, "{before}");

    // A click inside the opaque region must become a tap. The idle animation moves
    // the drawn model between frames, so the helper re-reads the region and clicks
    // its centre rather than trusting one stale point.
    let tapped = tap_opaque_point(&runtime, &mut fake, 0, largest_rect_centre);
    assert_eq!(tapped["right_clicks"], 0, "{tapped}");
    assert!(
        tapped["pointer_events"].as_u64().unwrap_or(0) >= 3,
        "the enter, motion and button pair must all be counted: {tapped}"
    );
    // Haru declares Head and Body. The C++ tests the point against each declared
    // hit area's drawable bounds (`LAppModel::HitTest` -> `IsHit`), and the
    // drawable vertices are not reachable through `cubism-render`, so the runtime
    // must say exactly that rather than inventing a part. Its documented fallback
    // only applies to models that declare no `HitAreas` at all, which is what the
    // C++ does too.
    assert_eq!(tapped["hit_areas_declared"], true, "{tapped}");
    assert_eq!(tapped["hit_test_available"], false, "{tapped}");
    assert_eq!(
        tapped["last_hit"],
        Value::Null,
        "an unplaceable hit area must not claim a part: {tapped}"
    );
    assert!(
        tapped["interaction_error"]
            .as_str()
            .unwrap_or_default()
            .contains("HitAreas"),
        "the tap must report why it resolved no part: {tapped}"
    );
    assert_eq!(
        tapped["expressions_started"], 0,
        "an unplaceable hit area must not start an expression: {tapped}"
    );
    let taps_after_inside = count(&tapped, "taps");
    eprintln!("live: inside tap taps={taps_after_inside} declared=true available=false");

    // A transparent corner is click-through, so the compositor never delivers the
    // press to the overlay. The counters prove nothing was tapped.
    let corner = (1, 1);
    let before_corner = runtime.request(json!({"command":"get_status"}));
    let corner_frame = runtime.settled_frame(None);
    assert!(
        !region_rects(&corner_frame)
            .iter()
            .any(|&(x, y, w, h)| corner.0 >= x
                && corner.0 < x + w
                && corner.1 >= y
                && corner.1 < y + h),
        "the corner must really be outside the input region: {corner_frame}"
    );
    fake.click(BTN_LEFT, corner.0, corner.1);
    std::thread::sleep(Duration::from_millis(700));
    let after_corner = runtime.request(json!({"command":"get_status"}));
    assert_eq!(
        count(&after_corner, "taps"),
        count(&before_corner, "taps"),
        "a click in the transparent corner must not tap the model: {after_corner}"
    );
    assert_eq!(
        after_corner["last_hit"], before_corner["last_hit"],
        "a click outside the region must not change the last hit: {after_corner}"
    );
    eprintln!(
        "live: corner click left taps at {}",
        count(&after_corner, "taps")
    );

    // A right click is the C++ `next_model`, so the reported model changes.
    assert_eq!(after_corner["model"], "Alpha", "{after_corner}");
    // A right click is routed only when the press lands in the region, so click
    // the current opaque centre again rather than the stale one.
    let right_frame = runtime.settled_frame(None);
    let (inside_x, inside_y) = largest_rect_centre(&right_frame);
    fake.click(BTN_RIGHT, inside_x, inside_y);
    let switched = runtime.settled_frame_where(None, |reply| {
        reply["right_clicks"].as_u64().unwrap_or(0) > 0
    });
    assert_eq!(switched["right_clicks"], 1, "{switched}");
    let status = runtime.request(json!({"command":"get_status"}));
    assert_eq!(
        status["model"], "Beta",
        "a right click must advance the model: {status}"
    );
    assert_eq!(status["renderer_available"], true, "{status}");
    eprintln!("live: right click switched model to {}", status["model"]);

    // Hidden models ignore clicks exactly like the C++ `hidden && !screenEditing`
    // early return. The input region is empty while hidden, but the counter proves
    // the runtime refuses to act even if a press still arrived.
    let hidden = runtime.request(json!({"command":"toggle_hidden"}));
    assert_eq!(hidden["hidden"], true, "{hidden}");
    let hidden_frame =
        runtime.settled_frame_where(None, |reply| reply["visible_pixels"].as_u64() == Some(0));
    assert_eq!(count(&hidden_frame, "input_regions"), 0, "{hidden_frame}");
    let taps_before_hidden = count(&hidden_frame, "taps");
    // The region is empty while hidden, so the compositor routes this press below
    // the overlay; the runtime must still count it as ignored rather than tapping.
    fake.click(BTN_LEFT, inside_x, inside_y);
    std::thread::sleep(Duration::from_millis(700));
    let after_hidden = runtime.request(json!({"command":"get_status"}));
    assert_eq!(
        count(&after_hidden, "taps"),
        taps_before_hidden,
        "a hidden model must not tap: {after_hidden}"
    );
    eprintln!(
        "live: hidden taps stayed {}, ignored_hidden={}",
        count(&after_hidden, "taps"),
        count(&after_hidden, "ignored_hidden")
    );

    // Showing it again restores input, and a tap works again, so the hidden case
    // was a real guard rather than a one-way failure.
    let shown = runtime.request(json!({"command":"toggle_hidden"}));
    assert_eq!(shown["hidden"], false, "{shown}");
    let restored = runtime.settled_frame(None);
    assert!(count(&restored, "input_regions") > 0, "{restored}");
    let taps_before_restore = count(&restored, "taps");
    let restored_tap = tap_opaque_point(
        &runtime,
        &mut fake,
        taps_before_restore,
        largest_rect_centre,
    );
    assert!(
        count(&restored_tap, "taps") > taps_before_restore,
        "a shown model must tap again: {restored_tap}"
    );
    eprintln!(
        "live: restored taps={} pointer_events={}",
        count(&restored_tap, "taps"),
        count(&restored_tap, "pointer_events")
    );
    runtime.quit();
}

/// The C++ fallback hit test and tap dispatch, on a real model that declares no
/// `HitAreas`.
///
/// `LAppModel::HitTest` falls back to `Head` above the model midline and `Body`
/// below it only when `GetHitAreasCount() == 0`, so this uses the same Haru moc,
/// textures, expressions and `TapBody` motions with that one `model3` section
/// removed. A tap must then start a real expression on `Head` and a real
/// `TapBody` motion plus an expression on `Body`.
#[test]
fn overlay_tap_dispatches_real_expressions_on_a_model_without_hit_areas() {
    if !live_test_enabled("overlay_tap_dispatches_real_expressions_on_a_model_without_hit_areas") {
        return;
    }
    let _compositor_lock = lock_compositor();
    let compositor = Compositor::spawn();
    let fixture = fixture();
    let dir = tempfile::tempdir().unwrap();
    model_dir_without_hit_areas(dir.path(), "Fallback", &fixture);
    let runtime = Runtime::start(
        &compositor,
        &["--models-dir".as_ref(), dir.path().as_os_str()],
    );
    let mut fake = fake_input::FakeInput::new(compositor.connect());
    fake.move_absolute(1, 1);

    let tapped = tap_opaque_point(&runtime, &mut fake, 0, largest_rect_centre);
    assert_eq!(tapped["hit_areas_declared"], false, "{tapped}");
    assert_eq!(
        tapped["hit_test_available"], true,
        "the documented fallback must be usable without HitAreas: {tapped}"
    );
    let hit = tapped["last_hit"].as_str().unwrap_or_default();
    assert!(
        hit == "Head" || hit == "Body",
        "the fallback must resolve Head or Body: {tapped}"
    );
    assert_eq!(tapped["interaction_error"], Value::Null, "{tapped}");
    // `Head` starts one expression; `Body` starts a `TapBody` motion and an
    // expression. Both are real starts of the files the model actually declares.
    let expressions = count(&tapped, "expressions_started");
    let motions = count(&tapped, "motions_started");
    assert!(expressions >= 1, "a tap must start an expression: {tapped}");
    if hit == "Body" {
        assert!(motions >= 1, "a body tap must start TapBody: {tapped}");
    }
    eprintln!("live: fallback tap hit={hit} expressions={expressions} motions={motions}");

    // A second tap near the bottom of the model must resolve `Body`, which is the
    // other half of the fallback, and start both a `TapBody` motion and an
    // expression. If the model is posed so low that the bottommost band is still
    // above the midline, no honest `Body` assertion can be made; the test then
    // reports that instead of pretending either way.
    let second = tap_opaque_point(&runtime, &mut fake, count(&tapped, "taps"), |rects| {
        region_rects(rects)
            .into_iter()
            .max_by_key(|&(_, y, _, _)| y)
            .map(|(x, y, width, height)| (x + width / 2, y + height / 2))
            .unwrap_or_else(|| largest_rect_centre(rects))
    });
    let other_hit = second["last_hit"].as_str().unwrap_or_default();
    assert!(
        other_hit == "Head" || other_hit == "Body",
        "the second tap must resolve a part: {second}"
    );
    assert!(
        count(&second, "expressions_started") > expressions,
        "the second tap must start another expression: {second}"
    );
    if other_hit == "Body" {
        assert!(
            count(&second, "motions_started") >= 1,
            "a body tap must start a TapBody motion: {second}"
        );
    }
    eprintln!(
        "live: fallback taps={} hits={hit},{other_hit} expressions={} motions={}",
        count(&second, "taps"),
        count(&second, "expressions_started"),
        count(&second, "motions_started")
    );
    runtime.quit();
}

/// An Add expression is a layer over the motion base, not a new base for the
/// next frame. Exercise Scene::update through real taps, SDK blending and drawing.
#[test]
fn overlay_add_expression_does_not_accumulate_across_frames() {
    if !live_test_enabled("overlay_add_expression_does_not_accumulate_across_frames") {
        return;
    }
    let _compositor_lock = lock_compositor();
    let compositor = Compositor::spawn();
    let fixture = fixture();
    let dir = tempfile::tempdir().unwrap();
    let target = model_dir_without_hit_areas(dir.path(), "AddExpression", &fixture);
    let model_json = target.join(fixture.file_name().unwrap());
    let mut document: Value = serde_json::from_slice(&fs::read(&model_json).unwrap()).unwrap();
    // Keep Haru's real moc/textures, but isolate expression blending from motions
    // and eye blink. Do not use set_parameter: its persistent override would mask
    // the expression output and make this regression pass for the wrong reason.
    document["FileReferences"]["Motions"] = json!({});
    document.as_object_mut().unwrap().remove("Groups");
    document["FileReferences"]["Expressions"] =
        json!([{"Name":"AddAngle","File":"add-angle.exp3.json"}]);
    fs::write(model_json, serde_json::to_vec(&document).unwrap()).unwrap();
    fs::write(
        target.join("add-angle.exp3.json"),
        serde_json::to_vec_pretty(&json!({
            "Type":"Live2D Expression", "FadeInTime":0.0, "FadeOutTime":0.0,
            "Parameters":[{"Id":"ParamAngleX","Value":3.0,"Blend":"Add"}]
        }))
        .unwrap(),
    )
    .unwrap();
    let runtime = Runtime::start(
        &compositor,
        &["--models-dir".as_ref(), dir.path().as_os_str()],
    );
    let before = runtime.settled_frame(Some("ParamAngleX"));
    let base = before["parameter_value"].as_f64().unwrap();
    let mut fake = fake_input::FakeInput::new(compositor.connect());
    fake.move_absolute(1, 1);
    let mut taps = 0;
    for started in 1..=3 {
        let tapped = tap_opaque_point(&runtime, &mut fake, taps, |frame| {
            // With the identity view, y < height/2 is the fallback Head hit:
            // it starts the expression without trying a deliberately absent motion.
            region_rects(frame)
                .into_iter()
                .filter(|&(_, y, _, h)| y + h / 2 < count(frame, "height") as i32 / 2)
                .max_by_key(|&(_, _, w, h)| w * h)
                .map(|(x, y, w, h)| (x + w / 2, y + h / 2))
                .expect("Haru must have opaque head pixels above the model midline")
        });
        taps = count(&tapped, "taps");
        assert_eq!(count(&tapped, "expressions_started"), started, "{tapped}");
        assert_eq!(tapped["last_hit"], "Head", "{tapped}");
        assert_eq!(tapped["interaction_error"], Value::Null, "{tapped}");
        let mut checksum = None;
        let mut previous_frame = count(&before, "frames_drawn");
        for _ in 0..12 {
            let frame = runtime.settled_frame(Some("ParamAngleX"));
            let value = frame["parameter_value"].as_f64().unwrap();
            assert!(
                (value - (base + 3.0)).abs() < 1e-5,
                "Add must apply once per frame, not feed back into the base: {frame}"
            );
            assert!(count(&frame, "frames_drawn") > previous_frame, "{frame}");
            previous_frame = count(&frame, "frames_drawn");
            let pixels = count(&frame, "pixel_checksum");
            assert_ne!(pixels, count(&before, "pixel_checksum"), "{frame}");
            if let Some(expected) = checksum {
                assert_eq!(
                    pixels, expected,
                    "the isolated expression pose must stay stable: {frame}"
                );
            }
            checksum = Some(pixels);
        }
    }
    runtime.quit();
}

/// The centre of the tallest input-region rectangle of a reply, so a click lands
/// on opaque model pixels rather than the transparent margin.
fn largest_rect_centre(reply: &Value) -> (i32, i32) {
    region_rects(reply)
        .into_iter()
        .max_by_key(|&(_, _, _, height)| height)
        .map(|(x, y, width, height)| (x + width / 2, y + height / 2))
        .unwrap_or_else(|| panic!("no input region to click in: {reply}"))
}

/// Clicks a real opaque point until the runtime's tap counter exceeds `before`.
///
/// The idle animation moves the drawn model between frames, so the region the
/// point was chosen from can be stale by the time the compositor routes the
/// press. Each attempt re-reads the current region and clicks its centre, which
/// keeps "an opaque click becomes a tap" a real assertion without depending on a
/// frozen pose.
fn tap_opaque_point(
    runtime: &Runtime,
    fake: &mut fake_input::FakeInput,
    before: u64,
    pick: impl Fn(&Value) -> (i32, i32),
) -> Value {
    let deadline = Instant::now() + TIMEOUT;
    let mut last = Value::Null;
    while Instant::now() < deadline {
        let frame = runtime.settled_frame(None);
        let (x, y) = pick(&frame);
        fake.click(BTN_LEFT, x, y);
        std::thread::sleep(Duration::from_millis(500));
        let reply = runtime.request(json!({"command":"get_status"}));
        if reply["taps"].as_u64().unwrap_or(0) > before {
            return reply;
        }
        last = reply;
    }
    panic!("no tap within {TIMEOUT:?} after clicking an opaque point: {last}");
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
