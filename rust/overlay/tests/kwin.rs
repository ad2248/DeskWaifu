//! Real compositor proof for the overlay crate.
//!
//! Every live test is opt-in through `DESKWAIFU_OVERLAY_LIVE_TEST=1`. Without
//! it each live test prints one skip line and returns, so the ordinary
//! `cargo test` run, including the workspace run, stays green. With it set the
//! test spawns a private `kwin_wayland --virtual` instance and every assertion
//! below is mandatory; a missing `kwin_wayland` fails loudly.
//!
//! The tests share process-wide `XDG_RUNTIME_DIR`/`WAYLAND_DISPLAY` variables,
//! so they are serialized by [`COMPOSITOR_LOCK`].

use std::ffi::c_void;
use std::fs;
use std::io::Write;
use std::os::unix::fs::PermissionsExt;
use std::os::unix::io::{AsRawFd, BorrowedFd};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::{Mutex, MutexGuard};
use std::time::{Duration, Instant};

use deskwaifu_overlay::{
    Anchors, KeyboardInteractivity, Layer, Overlay, OverlayConfig, PointerEvent, Rect,
};
use wayland_client::globals::{registry_queue_init, GlobalListContents};
use wayland_client::protocol::{
    wl_buffer, wl_compositor, wl_pointer, wl_region, wl_registry, wl_seat, wl_shm, wl_shm_pool,
    wl_surface,
};
use wayland_client::{Connection, Dispatch, EventQueue, QueueHandle};
use wayland_protocols_wlr::layer_shell::v1::client::{zwlr_layer_shell_v1, zwlr_layer_surface_v1};

mod fake_input {
    //! Bindings for the KWin fake-input protocol used as a test fixture.
    #![allow(unused_imports)]

    // The generated code refers to `super::wayland_client`.
    #[allow(clippy::single_component_path_imports)]
    use wayland_client;

    pub mod __interfaces {
        wayland_scanner::generate_interfaces!("tests/fake-input.xml");
    }
    use self::__interfaces::*;

    wayland_scanner::generate_client_code!("tests/fake-input.xml");
}

use fake_input::org_kde_kwin_fake_input::{self, OrgKdeKwinFakeInput};

/// Bounded deadline for every asynchronous stage.
const TIMEOUT: Duration = Duration::from_secs(10);
const WIDTH: u32 = 800;
const HEIGHT: u32 = 600;
const BTN_LEFT: u32 = 0x110;
const PRESSED: u32 = 1;
const RELEASED: u32 = 0;
/// The `org_kde_kwin_fake_input.axis` value used as one wheel notch, in the
/// surface-local units of `wl_pointer.axis`.
const SCROLL_UNITS: f64 = 10.0;

static COMPOSITOR_LOCK: Mutex<()> = Mutex::new(());

/// Environment variable that opts into the live KWin compositor tests.
const LIVE_TEST_ENV: &str = "DESKWAIFU_OVERLAY_LIVE_TEST";

fn lock_compositor() -> MutexGuard<'static, ()> {
    COMPOSITOR_LOCK
        .lock()
        .unwrap_or_else(|error| error.into_inner())
}

/// Returns `true` when the live tests should run, `false` after printing the
/// single skip line. With the variable set, a missing `kwin_wayland` panics.
fn live_test_enabled() -> bool {
    if std::env::var_os(LIVE_TEST_ENV).as_deref() != Some("1".as_ref()) {
        // Written straight to fd 2 because libtest captures `eprintln!` of a
        // passing test; the skip must stay visible in an ordinary run.
        let message =
            format!("skipped: set {LIVE_TEST_ENV}=1 to run the real KWin compositor test\n");
        unsafe {
            libc::write(2, message.as_ptr().cast(), message.len());
        }
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

/// A private `kwin_wayland --virtual` instance, killed and reaped on drop.
struct Compositor {
    child: Child,
    runtime_dir: PathBuf,
    previous_runtime_dir: Option<String>,
    previous_display: Option<String>,
}

impl Compositor {
    fn spawn() -> Compositor {
        let runtime_dir = tempfile::Builder::new()
            .prefix("deskwaifu-overlay-")
            .tempdir()
            .expect("create private XDG_RUNTIME_DIR")
            .keep();
        fs::set_permissions(&runtime_dir, fs::Permissions::from_mode(0o700))
            .expect("chmod private XDG_RUNTIME_DIR");
        let socket_name = format!("deskwaifu-overlay-{}", std::process::id());

        let child = Command::new("kwin_wayland")
            .args([
                "--virtual",
                "--no-lockscreen",
                "--no-global-shortcuts",
                "--no-kactivities",
                "--width",
                &WIDTH.to_string(),
                "--height",
                &HEIGHT.to_string(),
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

        let mut child = child;
        let socket = runtime_dir.join(&socket_name);
        let deadline = Instant::now() + TIMEOUT;
        while Instant::now() < deadline {
            if socket.exists() {
                let previous_runtime_dir = std::env::var("XDG_RUNTIME_DIR").ok();
                let previous_display = std::env::var("WAYLAND_DISPLAY").ok();
                // In-process connection environment, restored by `Drop`.
                unsafe {
                    std::env::set_var("XDG_RUNTIME_DIR", &runtime_dir);
                    std::env::set_var("WAYLAND_DISPLAY", &socket_name);
                }
                return Compositor {
                    child,
                    runtime_dir,
                    previous_runtime_dir,
                    previous_display,
                };
            }
            std::thread::sleep(Duration::from_millis(20));
        }
        let _ = child.kill();
        let _ = child.wait();
        panic!(
            "kwin_wayland did not create {} within {TIMEOUT:?}",
            socket.display()
        );
    }

    fn runtime_dir(&self) -> &Path {
        &self.runtime_dir
    }

    fn connect(&self) -> Connection {
        Connection::connect_to_env()
            .unwrap_or_else(|error| panic!("connect to private compositor: {error}"))
    }
}

impl Drop for Compositor {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
        match &self.previous_runtime_dir {
            Some(value) => unsafe { std::env::set_var("XDG_RUNTIME_DIR", value) },
            None => unsafe { std::env::remove_var("XDG_RUNTIME_DIR") },
        }
        match &self.previous_display {
            Some(value) => unsafe { std::env::set_var("WAYLAND_DISPLAY", value) },
            None => unsafe { std::env::remove_var("WAYLAND_DISPLAY") },
        }
        let _ = fs::remove_dir_all(self.runtime_dir());
    }
}

/// Counts pointer button presses on the surface it observes.
#[derive(Default)]
struct PointerCount {
    presses: u32,
    enters: u32,
    pointer: Option<wl_pointer::WlPointer>,
}

impl Dispatch<wl_registry::WlRegistry, GlobalListContents> for PointerCount {
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

impl Dispatch<wl_seat::WlSeat, ()> for PointerCount {
    fn event(
        state: &mut Self,
        seat: &wl_seat::WlSeat,
        event: wl_seat::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        if let wl_seat::Event::Capabilities {
            capabilities: wayland_client::WEnum::Value(capabilities),
        } = event
        {
            if capabilities.contains(wl_seat::Capability::Pointer) && state.pointer.is_none() {
                state.pointer = Some(seat.get_pointer(qh, ()));
            }
        }
    }
}

impl Dispatch<wl_pointer::WlPointer, ()> for PointerCount {
    fn event(
        state: &mut Self,
        _: &wl_pointer::WlPointer,
        event: wl_pointer::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            wl_pointer::Event::Enter { .. } => state.enters += 1,
            wl_pointer::Event::Button {
                button: BTN_LEFT,
                state: wayland_client::WEnum::Value(wl_pointer::ButtonState::Pressed),
                ..
            } => state.presses += 1,
            _ => {}
        }
    }
}

/// A pointer observer on its own connection, plus its event queue.
struct Pointer {
    connection: Connection,
    queue: EventQueue<PointerCount>,
    count: PointerCount,
}

impl Pointer {
    fn new(connection: Connection) -> Pointer {
        let (globals, mut queue) = registry_queue_init::<PointerCount>(&connection)
            .expect("registry for pointer observer");
        let qh = queue.handle();
        let seat: wl_seat::WlSeat = globals
            .bind(&qh, 1..=7, ())
            .expect("compositor must advertise wl_seat");
        let _ = seat;
        let mut count = PointerCount::default();
        queue.roundtrip(&mut count).expect("seat capabilities");
        assert!(
            count.pointer.is_some(),
            "the private compositor must expose a pointer capability"
        );
        Pointer {
            connection,
            queue,
            count,
        }
    }

    /// Read and dispatch pending events. A round-trip is used because
    /// `dispatch_pending` only sees events another dispatch already read.
    fn pump(&mut self) {
        self.connection.flush().expect("flush pointer observer");
        self.queue
            .roundtrip(&mut self.count)
            .expect("dispatch pointer events");
    }
}

/// An opaque background layer surface with an input region covering everything.
struct Below {
    connection: Connection,
    queue: EventQueue<BelowState>,
    state: BelowState,
}

struct BelowState {
    compositor: wl_compositor::WlCompositor,
    layer_shell: zwlr_layer_shell_v1::ZwlrLayerShellV1,
    seat: wl_seat::WlSeat,
    pointer: Option<wl_pointer::WlPointer>,
    surface: wl_surface::WlSurface,
    layer_surface: Option<zwlr_layer_surface_v1::ZwlrLayerSurfaceV1>,
    configured: bool,
    closed: bool,
    presses: u32,
    enters: u32,
}

impl Below {
    fn new(connection: Connection) -> Below {
        let (globals, mut queue) = registry_queue_init::<BelowState>(&connection)
            .expect("registry for background surface");
        let qh = queue.handle();
        let compositor: wl_compositor::WlCompositor =
            globals.bind(&qh, 1..=4, ()).expect("wl_compositor");
        let layer_shell: zwlr_layer_shell_v1::ZwlrLayerShellV1 =
            globals.bind(&qh, 1..=4, ()).expect("zwlr_layer_shell_v1");
        let shm: wl_shm::WlShm = globals.bind(&qh, 1..=1, ()).expect("wl_shm");
        let seat: wl_seat::WlSeat = globals.bind(&qh, 1..=7, ()).expect("wl_seat");

        let surface = compositor.create_surface(&qh, ());
        let layer_surface = layer_shell.get_layer_surface(
            &surface,
            None,
            zwlr_layer_shell_v1::Layer::Background,
            "deskwaifu-overlay-test-below".to_string(),
            &qh,
            (),
        );
        layer_surface.set_size(WIDTH, HEIGHT);
        layer_surface.set_anchor(zwlr_layer_surface_v1::Anchor::Top);
        layer_surface.set_exclusive_zone(-1);
        surface.commit();

        let mut state = BelowState {
            compositor,
            layer_shell,
            seat,
            pointer: None,
            surface,
            layer_surface: Some(layer_surface),
            configured: false,
            closed: false,
            presses: 0,
            enters: 0,
        };
        // `roundtrip` always returns: the compositor answers the sync request,
        // unlike `blocking_dispatch`, which could wait forever.
        let deadline = Instant::now() + TIMEOUT;
        while !state.configured && Instant::now() < deadline {
            queue
                .roundtrip(&mut state)
                .expect("background surface configure");
        }
        assert!(
            state.configured,
            "background layer surface never received a configure"
        );
        let deadline = Instant::now() + TIMEOUT;
        while state.pointer.is_none() && Instant::now() < deadline {
            queue
                .roundtrip(&mut state)
                .expect("background surface seat capabilities");
        }
        assert!(
            state.pointer.is_some(),
            "background surface must observe pointer input"
        );

        // Opaque input region covering the whole surface.
        let region = state.compositor.create_region(&qh, ());
        region.add(0, 0, WIDTH as i32, HEIGHT as i32);
        state.surface.set_input_region(Some(&region));
        region.destroy();

        let buffer = opaque_buffer(&shm, &qh);
        state.surface.attach(Some(&buffer), 0, 0);
        state.surface.damage(0, 0, WIDTH as i32, HEIGHT as i32);
        state.surface.commit();
        queue.roundtrip(&mut state).expect("map background surface");

        Below {
            connection,
            queue,
            state,
        }
    }

    /// Read and dispatch pending events, including the compositor's input.
    fn pump(&mut self) {
        self.connection.flush().expect("flush background surface");
        self.queue
            .roundtrip(&mut self.state)
            .expect("dispatch background surface");
    }
}

/// A `wl_shm` buffer filled with opaque green.
fn opaque_buffer(shm: &wl_shm::WlShm, qh: &QueueHandle<BelowState>) -> wl_buffer::WlBuffer {
    let stride = (WIDTH * 4) as i32;
    let size = stride * HEIGHT as i32;
    let mut file = tempfile::tempfile().expect("create shm backing file");
    let mut pixels = vec![0u8; size as usize];
    for pixel in pixels.chunks_mut(4) {
        pixel.copy_from_slice(&[0x00, 0xff, 0x00, 0xff]);
    }
    file.write_all(&pixels).expect("fill shm backing file");
    let fd = unsafe { BorrowedFd::borrow_raw(file.as_raw_fd()) };
    let pool = shm.create_pool(fd, size, qh, ());
    let buffer = pool.create_buffer(
        0,
        WIDTH as i32,
        HEIGHT as i32,
        stride,
        wl_shm::Format::Argb8888,
        qh,
        (),
    );
    pool.destroy();
    buffer
}

impl Dispatch<wl_registry::WlRegistry, GlobalListContents> for BelowState {
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

impl Dispatch<wl_compositor::WlCompositor, ()> for BelowState {
    fn event(
        _: &mut Self,
        _: &wl_compositor::WlCompositor,
        _: wl_compositor::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wl_shm::WlShm, ()> for BelowState {
    fn event(
        _: &mut Self,
        _: &wl_shm::WlShm,
        _: wl_shm::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wl_region::WlRegion, ()> for BelowState {
    fn event(
        _: &mut Self,
        _: &wl_region::WlRegion,
        _: wl_region::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wl_shm_pool::WlShmPool, ()> for BelowState {
    fn event(
        _: &mut Self,
        _: &wl_shm_pool::WlShmPool,
        _: wl_shm_pool::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wl_buffer::WlBuffer, ()> for BelowState {
    fn event(
        _: &mut Self,
        _: &wl_buffer::WlBuffer,
        _: wl_buffer::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wl_surface::WlSurface, ()> for BelowState {
    fn event(
        _: &mut Self,
        _: &wl_surface::WlSurface,
        _: wl_surface::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<zwlr_layer_shell_v1::ZwlrLayerShellV1, ()> for BelowState {
    fn event(
        _: &mut Self,
        _: &zwlr_layer_shell_v1::ZwlrLayerShellV1,
        _: zwlr_layer_shell_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<zwlr_layer_surface_v1::ZwlrLayerSurfaceV1, ()> for BelowState {
    fn event(
        state: &mut Self,
        layer_surface: &zwlr_layer_surface_v1::ZwlrLayerSurfaceV1,
        event: zwlr_layer_surface_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            zwlr_layer_surface_v1::Event::Configure { serial, .. } => {
                layer_surface.ack_configure(serial);
                state.configured = true;
            }
            zwlr_layer_surface_v1::Event::Closed => state.closed = true,
            _ => {}
        }
    }
}

impl Dispatch<wl_seat::WlSeat, ()> for BelowState {
    fn event(
        state: &mut Self,
        seat: &wl_seat::WlSeat,
        event: wl_seat::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        if let wl_seat::Event::Capabilities {
            capabilities: wayland_client::WEnum::Value(capabilities),
        } = event
        {
            if capabilities.contains(wl_seat::Capability::Pointer) && state.pointer.is_none() {
                state.pointer = Some(seat.get_pointer(qh, ()));
            }
        }
    }
}

impl Dispatch<wl_pointer::WlPointer, ()> for BelowState {
    fn event(
        state: &mut Self,
        _: &wl_pointer::WlPointer,
        event: wl_pointer::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            wl_pointer::Event::Enter { .. } => state.enters += 1,
            wl_pointer::Event::Button {
                button: BTN_LEFT,
                state: wayland_client::WEnum::Value(wl_pointer::ButtonState::Pressed),
                ..
            } => state.presses += 1,
            _ => {}
        }
    }
}

impl Drop for BelowState {
    fn drop(&mut self) {
        if let Some(layer_surface) = self.layer_surface.take() {
            layer_surface.destroy();
        }
        self.surface.destroy();
        let _ = &self.layer_shell;
        let _ = &self.seat;
    }
}

/// The KWin fake-input device, authenticated on its own connection.
struct FakeInput {
    connection: Connection,
    queue: EventQueue<FakeInputState>,
    device: OrgKdeKwinFakeInput,
    state: FakeInputState,
}

#[derive(Default)]
struct FakeInputState {}

impl FakeInput {
    fn new(connection: Connection) -> FakeInput {
        let (globals, mut queue) =
            registry_queue_init::<FakeInputState>(&connection).expect("registry for fake input");
        let qh = queue.handle();
        let device: OrgKdeKwinFakeInput = globals
            .bind(&qh, 1..=3, ())
            .expect("KWin must advertise org_kde_kwin_fake_input v3 for real input injection");
        let mut state = FakeInputState::default();
        device.authenticate(
            "DeskWaifu overlay test".to_string(),
            "private headless click-through proof".to_string(),
        );
        queue
            .roundtrip(&mut state)
            .expect("authenticate fake input");
        FakeInput {
            connection,
            queue,
            device,
            state,
        }
    }

    fn move_absolute(&mut self, x: i32, y: i32) {
        self.device
            .pointer_motion_absolute(f64::from(x), f64::from(y));
        self.connection.flush().expect("flush fake motion");
        self.queue
            .roundtrip(&mut self.state)
            .expect("motion completion barrier");
    }

    /// One vertical wheel notch, in `wl_pointer.axis` surface-local units.
    fn scroll(&mut self, x: i32, y: i32) {
        self.move_absolute(x, y);
        self.device.axis(0, SCROLL_UNITS);
        self.connection.flush().expect("flush fake axis");
        self.queue
            .roundtrip(&mut self.state)
            .expect("axis completion barrier");
    }

    fn click(&mut self, x: i32, y: i32) {
        self.move_absolute(x, y);
        self.device.button(BTN_LEFT, PRESSED);
        self.device.button(BTN_LEFT, RELEASED);
        self.connection.flush().expect("flush fake button");
        self.queue
            .roundtrip(&mut self.state)
            .expect("click completion barrier");
    }
}

impl Dispatch<org_kde_kwin_fake_input::OrgKdeKwinFakeInput, ()> for FakeInputState {
    fn event(
        _: &mut Self,
        _: &org_kde_kwin_fake_input::OrgKdeKwinFakeInput,
        _: org_kde_kwin_fake_input::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
    }
}

impl Dispatch<wl_registry::WlRegistry, GlobalListContents> for FakeInputState {
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

fn overlay_config(width: u32, height: u32) -> OverlayConfig {
    OverlayConfig {
        anchors: Anchors::TOP | Anchors::LEFT,
        layer: Layer::Overlay,
        exclusive_zone: -1,
        keyboard_interactivity: KeyboardInteractivity::None,
        ..OverlayConfig::new(width, height)
    }
}

/// Pump both client queues once, ignoring protocol errors that a closed
/// compositor would otherwise turn into dispatcher panics.
fn pump(below: &mut Below, overlay_pointer: &mut Pointer) {
    below.pump();
    overlay_pointer.pump();
}

/// Wait until `predicate` holds, re-sending the pointer position when given.
fn settle(
    below: &mut Below,
    overlay_pointer: &mut Pointer,
    mut fake: Option<&mut FakeInput>,
    position: Option<(i32, i32)>,
    predicate: impl Fn(&Below, &Pointer) -> bool,
) -> bool {
    let deadline = Instant::now() + TIMEOUT;
    while Instant::now() < deadline {
        if predicate(below, overlay_pointer) {
            return true;
        }
        if let (Some(fake), Some((x, y))) = (fake.as_deref_mut(), position) {
            fake.move_absolute(x, y);
        } else {
            std::thread::sleep(Duration::from_millis(10));
        }
        pump(below, overlay_pointer);
    }
    predicate(below, overlay_pointer)
}

#[test]
fn overlay_configures_and_renders_transparent() {
    if !live_test_enabled() {
        return;
    }
    let _guard = lock_compositor();
    let compositor = Compositor::spawn();

    let mut overlay = Overlay::connect(compositor.connect(), overlay_config(WIDTH, HEIGHT))
        .expect("create the overlay surface");
    let size = overlay.size();
    assert_eq!(
        (size.0 > 0, size.1 > 0),
        (true, true),
        "configure must provide a nonzero surface size, got {size:?}"
    );
    overlay.roundtrip().expect("roundtrip after configure");
    assert!(!overlay.is_closed(), "overlay surface must stay open");

    overlay
        .make_current()
        .expect("make the EGL context current");
    load_gl(&overlay);
    let (width, height) = (size.0 as i32, size.1 as i32);

    unsafe {
        gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
        gl::Viewport(0, 0, width, height);
        gl::ClearColor(0.0, 0.0, 0.0, 0.0);
        gl::Clear(gl::COLOR_BUFFER_BIT);
    }
    // Read the freshly drawn back buffer, then present it.
    let transparent = read_center_alpha(width, height);
    assert_eq!(
        gl_error(),
        gl::NO_ERROR,
        "GL error after the transparent frame"
    );
    overlay.swap_buffers().expect("swap the transparent frame");
    assert_eq!(transparent, 0, "a transparent clear must read back alpha 0");

    unsafe {
        gl::ClearColor(1.0, 0.0, 0.0, 1.0);
        gl::Clear(gl::COLOR_BUFFER_BIT);
    }
    // The back buffer holds the frame until the swap; after it, contents are
    // undefined, so both readbacks happen before presenting.
    let opaque = read_center_alpha(width, height);
    assert_eq!(gl_error(), gl::NO_ERROR, "GL error after the opaque frame");
    overlay.swap_buffers().expect("swap the opaque frame");
    assert_eq!(opaque, 255, "an opaque clear must read back alpha 255");

    overlay.dispatch_pending().expect("dispatch after drawing");
}

fn load_gl(overlay: &Overlay) {
    gl::load_with(|name| {
        overlay
            .egl()
            .get_proc_address(name)
            .map_or(std::ptr::null(), |function| function as *const c_void)
    });
}

fn read_center_alpha(width: i32, height: i32) -> u8 {
    let mut pixel = [0u8; 4];
    unsafe {
        gl::ReadPixels(
            width / 2,
            height / 2,
            1,
            1,
            gl::RGBA,
            gl::UNSIGNED_BYTE,
            pixel.as_mut_ptr().cast(),
        );
    }
    pixel[3]
}

fn gl_error() -> u32 {
    unsafe { gl::GetError() }
}

#[test]
fn overlay_click_through_follows_the_input_region() {
    if !live_test_enabled() {
        return;
    }
    let _guard = lock_compositor();
    let compositor = Compositor::spawn();

    let mut overlay = Overlay::connect(compositor.connect(), overlay_config(WIDTH, HEIGHT))
        .expect("create the overlay surface");
    overlay
        .make_current()
        .expect("make the EGL context current");
    load_gl(&overlay);
    unsafe {
        gl::ClearColor(0.0, 0.0, 0.0, 0.0);
        gl::Clear(gl::COLOR_BUFFER_BIT);
    }
    overlay.swap_buffers().expect("present the overlay frame");
    overlay.roundtrip().expect("map the overlay surface");

    let mut fake = FakeInput::new(compositor.connect());
    // KWin only materialises its fake pointer device once it is used, and the
    // seat advertises the capability only after that.
    fake.move_absolute(1, 1);
    let mut below = Below::new(compositor.connect());
    let mut overlay_pointer = Pointer::new(overlay.connection().clone());

    let center = (WIDTH as i32 / 2, HEIGHT as i32 / 2);

    // Stage 1: empty input region is fully click-through.
    assert!(
        settle(
            &mut below,
            &mut overlay_pointer,
            Some(&mut fake),
            Some(center),
            |below, _| below.state.enters > 0,
        ),
        "pointer never entered the background surface with an empty overlay input region"
    );
    fake.click(center.0, center.1);
    assert!(
        settle(
            &mut below,
            &mut overlay_pointer,
            Some(&mut fake),
            Some(center),
            |below, _| below.state.presses > 0,
        ),
        "empty input region leaked a click: overlay={} below={}",
        overlay_pointer.count.presses,
        below.state.presses
    );
    assert_eq!(
        overlay_pointer.count.presses, 0,
        "empty input region must not deliver a press to the overlay: overlay={} below={}",
        overlay_pointer.count.presses, below.state.presses
    );

    // Stage 2: a shaped region around the centre captures the click.
    overlay
        .set_input_region(&[Rect::new(center.0 - 100, center.1 - 100, 200, 200)])
        .expect("set the shaped input region");
    overlay.roundtrip().expect("apply the shaped input region");
    assert!(
        settle(
            &mut below,
            &mut overlay_pointer,
            Some(&mut fake),
            Some(center),
            |_, overlay| overlay.count.enters > 0,
        ),
        "pointer never entered the overlay with a shaped input region: overlay_enters={} below_enters={}",
        overlay_pointer.count.enters,
        below.state.enters
    );
    let below_before = below.state.presses;
    fake.click(center.0, center.1);
    assert!(
        settle(
            &mut below,
            &mut overlay_pointer,
            Some(&mut fake),
            Some(center),
            |_, overlay| overlay.count.presses > 0,
        ),
        "shaped input region did not deliver a press to the overlay: overlay={} below={}",
        overlay_pointer.count.presses,
        below.state.presses
    );
    pump(&mut below, &mut overlay_pointer);
    assert_eq!(
        below.state.presses, below_before,
        "shaped input region leaked a click to the background: overlay={} below={}",
        overlay_pointer.count.presses, below.state.presses
    );

    // Stage 3: clearing the region restores click-through.
    overlay
        .set_input_region(&[])
        .expect("clear the input region");
    overlay.roundtrip().expect("apply the empty input region");
    let overlay_before = overlay_pointer.count.presses;
    let below_before = below.state.presses;
    assert!(
        settle(
            &mut below,
            &mut overlay_pointer,
            Some(&mut fake),
            Some(center),
            |below, _| below.state.enters > 1,
        ),
        "pointer never returned to the background surface: below_enters={}",
        below.state.enters
    );
    fake.click(center.0, center.1);
    assert!(
        settle(
            &mut below,
            &mut overlay_pointer,
            Some(&mut fake),
            Some(center),
            |below, _| below.state.presses > below_before,
        ),
        "cleared input region retained the click: overlay={} below={}",
        overlay_pointer.count.presses,
        below.state.presses
    );
    pump(&mut below, &mut overlay_pointer);
    assert_eq!(
        overlay_pointer.count.presses, overlay_before,
        "cleared input region still delivered a press to the overlay: overlay={} below={}",
        overlay_pointer.count.presses, below.state.presses
    );
}

#[test]
fn overlay_reports_pointer_events_and_respects_the_input_region() {
    if !live_test_enabled() {
        return;
    }
    let _guard = lock_compositor();
    let compositor = Compositor::spawn();

    let mut overlay = Overlay::connect(compositor.connect(), overlay_config(WIDTH, HEIGHT))
        .expect("create the overlay surface");
    overlay
        .make_current()
        .expect("make the EGL context current");
    load_gl(&overlay);
    unsafe {
        gl::ClearColor(0.0, 0.0, 0.0, 0.0);
        gl::Clear(gl::COLOR_BUFFER_BIT);
    }
    overlay.swap_buffers().expect("present the overlay frame");
    overlay.roundtrip().expect("map the overlay surface");

    let mut fake = FakeInput::new(compositor.connect());
    // KWin only materialises its fake pointer device once it is used, and the
    // seat advertises the capability only after that.
    fake.move_absolute(1, 1);
    let mut below = Below::new(compositor.connect());

    let center = (WIDTH as i32 / 2, HEIGHT as i32 / 2);
    let target = (center.0 + 40, center.1 + 30);
    let shaped = Rect::new(center.0 - 100, center.1 - 100, 200, 200);
    // The overlay is anchored Top|Left on the virtual output and requested at
    // the output size, so surface-local coordinates equal the compositor-global
    // ones `pointer_motion_absolute` takes.
    assert!(
        overlay.take_events().is_empty(),
        "a freshly connected overlay reports no pointer events"
    );

    // Stage 1: an empty input region reports nothing and sends the click below.
    let below_before = below.state.presses;
    fake.click(center.0, center.1);
    assert!(
        wait_for_below_press(&mut below, &mut overlay, below_before),
        "the empty input region did not send the click to the background surface"
    );
    let leaked = overlay.take_events();
    assert!(
        leaked.is_empty(),
        "an empty input region must not report pointer events to the overlay: {leaked:?}"
    );
    // Baseline for stage 2: the one press stage 1 sent below has arrived.
    let below_after_stage_one = below.state.presses;

    // Stage 2: a shaped region around the centre reports the pointer.
    overlay
        .set_input_region(&[shaped])
        .expect("set the shaped input region");
    overlay.roundtrip().expect("apply the shaped input region");
    // The pointer already sits at the centre, inside the new region, so the
    // compositor may enter on the next motion without any other prompt.
    let mut entered_events = overlay.take_events();
    entered_events.extend(overlay_events_until(
        &mut overlay,
        &mut fake,
        center,
        |events| {
            events
                .iter()
                .any(|event| matches!(event, PointerEvent::Entered { .. }))
        },
    ));
    let entered = entered_events
        .iter()
        .position(|event| matches!(event, PointerEvent::Entered { .. }))
        .unwrap_or_else(|| panic!("no PointerEntered arrived: {entered_events:?}"));
    assert_eq!(
        entered, 0,
        "the enter must be the first event: {entered_events:?}"
    );

    // A real move to a second point inside the region must report Moved.
    let mut move_events = overlay.take_events();
    move_events.extend(overlay_events_until(
        &mut overlay,
        &mut fake,
        target,
        |events| {
            events.iter().any(|event| {
                matches!(
                    *event,
                    PointerEvent::Moved { x, y } if near(x, target.0) && near(y, target.1)
                )
            })
        },
    ));
    let moved = move_events.iter().any(|event| {
        matches!(
            *event,
            PointerEvent::Moved { x, y } if near(x, target.0) && near(y, target.1)
        )
    });
    assert!(
        moved,
        "no PointerMoved at {target:?} arrived: {move_events:?}"
    );

    // A real left press and release at that point must report both.
    let mut click_events = overlay.take_events();
    fake.click(target.0, target.1);
    click_events.extend(overlay_events_until(
        &mut overlay,
        &mut fake,
        target,
        |events| {
            events.iter().any(|event| {
                matches!(
                    event,
                    PointerEvent::Button {
                        button: 0,
                        pressed: true
                    }
                )
            }) && events.iter().any(|event| {
                matches!(
                    event,
                    PointerEvent::Button {
                        button: 0,
                        pressed: false
                    }
                )
            })
        },
    ));
    let pressed = click_events
        .iter()
        .position(|event| {
            matches!(
                event,
                PointerEvent::Button {
                    button: 0,
                    pressed: true
                }
            )
        })
        .unwrap_or_else(|| panic!("no left press arrived: {click_events:?}"));
    let released = click_events
        .iter()
        .position(|event| {
            matches!(
                event,
                PointerEvent::Button {
                    button: 0,
                    pressed: false
                }
            )
        })
        .unwrap_or_else(|| panic!("no left release arrived: {click_events:?}"));
    assert!(
        pressed < released,
        "the left press must precede the release: {click_events:?}"
    );
    // A vertical wheel notch at that point must report one scroll in the
    // fixed-axis units KWin forwards, since the fake device sends no value120.
    let mut scroll_events = overlay.take_events();
    fake.scroll(target.0, target.1);
    scroll_events.extend(overlay_events_until(
        &mut overlay,
        &mut fake,
        target,
        |events| {
            events
                .iter()
                .any(|event| matches!(event, PointerEvent::Scroll { .. }))
        },
    ));
    let scrolled = scroll_events
        .iter()
        .find_map(|event| match *event {
            PointerEvent::Scroll { delta } => Some(delta),
            _ => None,
        })
        .unwrap_or_else(|| panic!("no PointerScroll arrived: {scroll_events:?}"));
    assert!(
        (scrolled - SCROLL_UNITS).abs() <= POINTER_TOLERANCE,
        "the fixed axis must be reported in its own units: {scrolled} vs {SCROLL_UNITS}"
    );
    assert_eq!(
        scroll_events
            .iter()
            .filter(|event| matches!(event, PointerEvent::Scroll { .. }))
            .count(),
        1,
        "one wheel notch must not report two scroll events: {scroll_events:?}"
    );

    for event in entered_events
        .iter()
        .chain(&move_events)
        .chain(&click_events)
    {
        match *event {
            PointerEvent::Entered { x, y } | PointerEvent::Moved { x, y } => {
                assert!(
                    inside(x, y, shaped),
                    "reported ({x}, {y}) outside the shaped input region {shaped:?}: {entered_events:?} {move_events:?} {click_events:?}"
                );
            }
            _ => {}
        }
    }
    // The press itself must not be swallowed by the background surface, so the
    // background must not see a newer one.
    below.pump();
    assert_eq!(
        below.state.presses, below_after_stage_one,
        "the shaped input region leaked the press below: below={}",
        below.state.presses
    );

    // Stage 3: clearing the region reports Left and sends the click below again.
    overlay
        .set_input_region(&[])
        .expect("clear the input region");
    overlay.roundtrip().expect("apply the empty input region");
    let mut left = overlay.take_events();
    left.extend(overlay_events_until(
        &mut overlay,
        &mut fake,
        center,
        |events| {
            events
                .iter()
                .any(|event| matches!(event, PointerEvent::Left))
        },
    ));
    assert!(
        left.contains(&PointerEvent::Left),
        "clearing the input region did not report PointerLeft: {left:?}"
    );
    let below_before = below.state.presses;
    fake.click(center.0, center.1);
    assert!(
        wait_for_below_press(&mut below, &mut overlay, below_before),
        "the cleared input region did not send the click to the background surface"
    );
    let leaked = overlay.take_events();
    assert!(
        leaked.is_empty(),
        "a cleared input region must not report pointer events to the overlay: {leaked:?}"
    );
}

/// Pixel tolerance for the live pointer assertions.
///
/// `org_kde_kwin_fake_input.pointer_motion_absolute` takes compositor-global
/// logical positions and the overlay surface has no fractional scale or
/// offset here, so the only error left is the compositor's own rounding.
const POINTER_TOLERANCE: f64 = 1.0;

fn near(reported: f64, injected: i32) -> bool {
    (reported - f64::from(injected)).abs() <= POINTER_TOLERANCE
}

/// Whether `(x, y)` lies inside `rect`, with the same tolerance used for
/// reported positions.
fn inside(x: f64, y: f64, rect: Rect) -> bool {
    let right = f64::from(rect.x + rect.width);
    let bottom = f64::from(rect.y + rect.height);
    x >= f64::from(rect.x) - POINTER_TOLERANCE
        && x <= right + POINTER_TOLERANCE
        && y >= f64::from(rect.y) - POINTER_TOLERANCE
        && y <= bottom + POINTER_TOLERANCE
}

/// Drain the overlay's own queue until `predicate` holds over the events
/// collected so far, re-sending `position` between rounds.
fn overlay_events_until(
    overlay: &mut Overlay,
    fake: &mut FakeInput,
    position: (i32, i32),
    predicate: impl Fn(&[PointerEvent]) -> bool,
) -> Vec<PointerEvent> {
    let mut events = Vec::new();
    let deadline = Instant::now() + TIMEOUT;
    loop {
        events.extend(overlay.take_events());
        if predicate(&events) || Instant::now() >= deadline {
            return events;
        }
        fake.move_absolute(position.0, position.1);
        overlay
            .roundtrip()
            .expect("dispatch overlay pointer events");
        std::thread::sleep(Duration::from_millis(10));
    }
}

/// Pump the background surface and the overlay until it sees another press.
fn wait_for_below_press(below: &mut Below, overlay: &mut Overlay, before: u32) -> bool {
    let deadline = Instant::now() + TIMEOUT;
    while Instant::now() < deadline {
        if below.state.presses > before {
            return true;
        }
        std::thread::sleep(Duration::from_millis(10));
        below.pump();
        overlay
            .roundtrip()
            .expect("dispatch overlay pointer events");
    }
    below.state.presses > before
}

#[test]
fn second_overlay_uses_its_own_connection() {
    if !live_test_enabled() {
        return;
    }
    let _guard = lock_compositor();
    let compositor = Compositor::spawn();

    let mut first = Overlay::connect(compositor.connect(), overlay_config(WIDTH, HEIGHT))
        .expect("first overlay");
    let mut second = Overlay::connect(compositor.connect(), overlay_config(320, 240))
        .expect("a second overlay must work on its own connection");
    first.roundtrip().expect("first overlay roundtrip");
    second.roundtrip().expect("second overlay roundtrip");
    assert!(!first.is_closed() && !second.is_closed());
    assert_eq!(second.size(), (320, 240));
}
