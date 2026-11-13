//! A Wayland layer-shell overlay surface with an EGL window surface.
//!
//! This crate is deliberately small: it owns one `wl_surface` with the
//! `zwlr_layer_surface_v1` role, its EGL context and window surface, the input
//! region that decides which pixels are clickable, and the `wl_pointer`
//! reporting for that surface. It contains no animation, model, or
//! coordinate-mapping logic; callers map their own rectangles into [`Rect`] and
//! do all GL drawing themselves. Tap/double-click detection, hit testing and
//! drag policy belong to the caller, as they do in the C++ window split.
//!
//! ```no_run
//! use deskwaifu_overlay::{Error, Overlay, OverlayConfig, PointerEvent, Rect};
//! use wayland_client::Connection;
//!
//! # fn example() -> Result<(), Error> {
//! let connection = Connection::connect_to_env()
//!     .map_err(|error| Error::Wayland(error.to_string()))?;
//! let mut overlay = Overlay::connect(connection, OverlayConfig::new(640, 480))?;
//! overlay.make_current()?;
//! overlay.set_input_region(&[Rect::new(10, 10, 40, 40)])?;
//! // Draw with the caller's own GL bindings, then:
//! overlay.swap_buffers()?;
//! overlay.dispatch_pending()?;
//! for event in overlay.take_events() {
//!     if let PointerEvent::Button { button, pressed } = event {
//!         println!("button {button} pressed={pressed}");
//!     }
//! }
//! # Ok(())
//! # }
//! ```

mod config;
mod egl_state;
mod error;
mod pointer;

pub use config::{resolve_configured_size, Anchors, KeyboardInteractivity, Layer, Rect};
pub use error::Error;
pub use pointer::PointerEvent;

use pointer::PointerFrame;

use std::ffi::c_void;
use std::marker::PhantomData;

use khronos_egl as egl;
use wayland_client::globals::{registry_queue_init, BindError, GlobalList, GlobalListContents};
use wayland_client::protocol::{
    wl_compositor, wl_output, wl_pointer, wl_region, wl_registry, wl_seat, wl_shm, wl_surface,
};
use wayland_client::{delegate_noop, Connection, Dispatch, EventQueue, Proxy, QueueHandle, WEnum};
use wayland_protocols_wlr::layer_shell::v1::client::{zwlr_layer_shell_v1, zwlr_layer_surface_v1};

use crate::egl_state::Egl;

/// Layer-shell namespace advertised to the compositor.
pub const NAMESPACE: &str = "deskwaifu-model";

/// Optional margins from the anchored edges, in pixels.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Margins {
    /// Space above the surface.
    pub top: i32,
    /// Space to the right of the surface.
    pub right: i32,
    /// Space below the surface.
    pub bottom: i32,
    /// Space to the left of the surface.
    pub left: i32,
}

/// Everything needed to create one overlay surface.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OverlayConfig {
    /// Surface width in pixels; must be greater than zero.
    pub width: u32,
    /// Surface height in pixels; must be greater than zero.
    pub height: u32,
    /// Anchor edges; the model window uses `Top | Left`.
    pub anchors: Anchors,
    /// Layer to place the surface on; the model window uses [`Layer::Overlay`].
    pub layer: Layer,
    /// Layer-shell exclusive zone; `-1` means "do not reserve space".
    pub exclusive_zone: i32,
    /// Keyboard interactivity; the model window uses
    /// [`KeyboardInteractivity::None`].
    pub keyboard_interactivity: KeyboardInteractivity,
    /// Optional margin from the anchored edges.
    pub margin: Option<Margins>,
    /// Optional `wl_output` name to pin the surface to. `None` lets the
    /// compositor decide, which is normally the output the user last interacted
    /// with; a name selects the output the caller considers current.
    pub output: Option<String>,
}

impl OverlayConfig {
    /// Model-window defaults: overlay layer, top-left anchor, exclusive zone
    /// `-1`, no keyboard interactivity, compositor-chosen output.
    pub fn new(width: u32, height: u32) -> OverlayConfig {
        OverlayConfig {
            width,
            height,
            anchors: Anchors::TOP | Anchors::LEFT,
            layer: Layer::Overlay,
            exclusive_zone: -1,
            keyboard_interactivity: KeyboardInteractivity::None,
            margin: None,
            output: None,
        }
    }
}

/// One layer-shell surface with its EGL context and input region.
///
/// Not `Send`/`Sync`: EGL and the Wayland proxies are bound to the thread that
/// created them.
pub struct Overlay {
    connection: Connection,
    queue: EventQueue<State>,
    state: State,
    not_send: PhantomData<*mut ()>,
}

impl Overlay {
    /// Connect the given Wayland connection, create the layer surface, wait for
    /// the first configure, then create the EGL context and window surface.
    ///
    /// The overlay starts fully click-through (an empty input region); call
    /// [`Overlay::set_input_region`] to accept pointer input.
    ///
    /// Each `Overlay` owns its `Connection`; a second overlay needs a second
    /// connection. The overlay binds one `wl_seat` for its pointer and reports
    /// that pointer's events through [`Overlay::take_events`]; callers that need
    /// other objects (for example a `wl_seat` of their own) can still create
    /// them on [`Overlay::connection`] with a separate [`EventQueue`].
    pub fn connect(connection: Connection, config: OverlayConfig) -> Result<Overlay, Error> {
        if config.width == 0 || config.height == 0 {
            return Err(Error::InvalidSize);
        }
        let requested_size = (config.width, config.height);
        let (globals, mut queue) = registry_queue_init::<State>(&connection)
            .map_err(|error| Error::Wayland(error.to_string()))?;
        let qh = queue.handle();
        let compositor = bind(&globals, &qh, "wl_compositor", 1..=4)?;
        let shm = bind(&globals, &qh, "wl_shm", 1..=1)?;
        let layer_shell: zwlr_layer_shell_v1::ZwlrLayerShellV1 =
            bind(&globals, &qh, "zwlr_layer_shell_v1", 1..=4)?;

        let mut state = State::new(requested_size, compositor, shm);
        // A compositor without `wl_seat` simply has no pointer to report: the
        // overlay still renders and still routes the input region. Version 8 is
        // the first `wl_pointer` version with `axis_value120`, so it is the
        // newest one this crate needs; older seats still work with the fixed
        // `axis` value.
        state.seat = globals
            .bind::<wl_seat::WlSeat, State, ()>(&qh, 1..=8, ())
            .ok();
        let output = match &config.output {
            Some(name) => Some(state.select_output(&globals, &qh, &mut queue, name)?),
            None => None,
        };

        let surface = state.compositor.create_surface(&qh, ());
        let layer_surface = layer_shell.get_layer_surface(
            &surface,
            output.as_ref(),
            protocol_layer(config.layer),
            NAMESPACE.to_string(),
            &qh,
            (),
        );
        layer_surface.set_size(config.width, config.height);
        layer_surface.set_anchor(protocol_anchor(config.anchors));
        layer_surface.set_exclusive_zone(config.exclusive_zone);
        layer_surface.set_keyboard_interactivity(protocol_keyboard(config.keyboard_interactivity));
        if let Some(margin) = config.margin {
            layer_surface.set_margin(margin.top, margin.right, margin.bottom, margin.left);
        }

        // Click-through from the start, like `SetInputRegion({ })`.
        let region = state.compositor.create_region(&qh, ());
        surface.set_input_region(Some(&region));
        state.region = Some(region);
        state.surface = Some(surface);
        state.layer_surface = Some(layer_surface);
        state.output = output;

        let surface = state.surface.as_ref().expect("surface was just set");
        surface.commit();
        queue
            .roundtrip(&mut state)
            .map_err(|error| Error::Wayland(error.to_string()))?;
        if state.closed {
            return Err(Error::Closed);
        }
        let size = state.configured.ok_or(Error::NotConfigured)?;

        let display_ptr = connection.backend().display_ptr().cast::<c_void>();
        let surface_ptr = state
            .surface
            .as_ref()
            .expect("surface was just set")
            .id()
            .as_ptr()
            .cast::<c_void>();
        state.egl = Some(Egl::new(display_ptr, surface_ptr, size)?);

        Ok(Overlay {
            connection,
            queue,
            state,
            not_send: PhantomData,
        })
    }

    /// The connection this overlay owns.
    pub fn connection(&self) -> &Connection {
        &self.connection
    }

    /// The configured surface size in pixels.
    pub fn size(&self) -> (u32, u32) {
        self.state
            .egl
            .as_ref()
            .map_or(self.state.requested_size, Egl::size)
    }

    /// Request a new layer-surface size. The compositor's subsequent configure
    /// event is authoritative; call `roundtrip` or `dispatch_pending` to receive it.
    pub fn request_size(&mut self, width: u32, height: u32) -> Result<(), Error> {
        if width == 0 || height == 0 {
            return Err(Error::InvalidSize);
        }
        self.state.requested_size = (width, height);
        let layer_surface = self.state.layer_surface.as_ref().ok_or(Error::Closed)?;
        layer_surface.set_size(width, height);
        self.state.surface.as_ref().ok_or(Error::Closed)?.commit();
        self.connection.flush()?;
        Ok(())
    }

    /// Whether the compositor asked for the surface to be closed.
    pub fn is_closed(&self) -> bool {
        self.state.closed
    }

    /// The `wl_shm` global bound for this connection.
    pub fn shm(&self) -> &wl_shm::WlShm {
        &self.state.shm
    }

    /// The EGL instance, for `eglGetProcAddress`-style GL loading.
    pub fn egl(&self) -> &egl::Instance<egl::Static> {
        self.egl_state()
            .expect("EGL is created by Overlay::connect")
            .instance()
    }

    /// The EGL display, for callers that need EGL entry points directly.
    pub fn display(&self) -> egl::Display {
        self.egl_state()
            .expect("EGL is created by Overlay::connect")
            .display()
    }

    /// Make the EGL context current on this thread.
    pub fn make_current(&self) -> Result<(), Error> {
        self.egl_state()?.make_current()
    }

    /// Present the drawn frame. Callers must make the context current first and
    /// draw transparent pixels with alpha 0.
    pub fn swap_buffers(&self) -> Result<(), Error> {
        self.egl_state()?.swap_buffers()
    }

    /// Replace the input region with the given surface-local rectangles.
    ///
    /// Rectangles are clipped to the surface; rectangles with a non-positive
    /// size or that fall outside it are dropped. An empty slice makes the whole
    /// surface click-through, matching `SetInputRegion({})` in the C++ window.
    pub fn set_input_region(&mut self, rects: &[Rect]) -> Result<(), Error> {
        let (width, height) = self.size();
        let qh = self.queue.handle();
        let surface = self.state.surface.as_ref().ok_or(Error::Closed)?.clone();
        let region = self.state.compositor.create_region(&qh, ());
        for rect in rects {
            if let Some(rect) = rect.clip(width, height) {
                region.add(rect.x, rect.y, rect.width, rect.height);
            }
        }
        surface.set_input_region(Some(&region));
        if let Some(old) = self.state.region.replace(region) {
            old.destroy();
        }
        surface.commit();
        self.connection.flush()?;
        Ok(())
    }

    /// Drain the pointer events collected since the last call.
    ///
    /// Events arrive here from [`Overlay::dispatch_pending`],
    /// [`Overlay::dispatch`] and [`Overlay::roundtrip`], in compositor order, in
    /// the same shape as the C++ `IDesktopWindowEvents` pointer callbacks:
    /// [`PointerEvent::Entered`], [`PointerEvent::Left`],
    /// [`PointerEvent::Moved`], [`PointerEvent::Button`] and
    /// [`PointerEvent::Scroll`]. Positions are surface-local pixels from the
    /// top-left corner, and `button` uses `0` left, `1` right, `2` middle.
    ///
    /// Nothing is synthesised: a frame's events are reported once, in order, and
    /// the buffer starts empty again. Tap/double-click detection and hit testing
    /// are the caller's job, matching the C++ `InteractionProcessor`.
    ///
    /// One deliberate difference from `QtDesktopWindow`: that window calls
    /// `PointerMoved` again before every button event, because Qt delivers a
    /// button with its own position. Here a position change is reported only by
    /// the `wl_pointer.motion` (or `enter`) that carried it, so a caller that
    /// needs the position of a click keeps the last `Moved`/`Entered` it saw.
    pub fn take_events(&mut self) -> Vec<PointerEvent> {
        std::mem::take(&mut self.state.events)
    }

    /// Flush outgoing requests and dispatch already-received events.
    pub fn dispatch_pending(&mut self) -> Result<(), Error> {
        self.connection.flush()?;
        self.queue
            .dispatch_pending(&mut self.state)
            .map_err(|error| Error::Wayland(error.to_string()))?;
        Ok(())
    }

    /// Block until at least one event arrives, then dispatch it.
    pub fn dispatch(&mut self) -> Result<(), Error> {
        self.queue
            .blocking_dispatch(&mut self.state)
            .map_err(|error| Error::Wayland(error.to_string()))?;
        Ok(())
    }

    /// Round-trip to the compositor and dispatch the resulting events.
    pub fn roundtrip(&mut self) -> Result<(), Error> {
        self.queue
            .roundtrip(&mut self.state)
            .map_err(|error| Error::Wayland(error.to_string()))?;
        Ok(())
    }

    fn egl_state(&self) -> Result<&Egl, Error> {
        self.state
            .egl
            .as_ref()
            .ok_or(Error::Unsupported("EGL is not initialized"))
    }
}

impl Drop for Overlay {
    fn drop(&mut self) {
        // EGL context/surface and the wl_egl_window go first.
        self.state.egl = None;
        if let Some(region) = self.state.region.take() {
            region.destroy();
        }
        if let Some(layer_surface) = self.state.layer_surface.take() {
            if layer_surface.version() >= 3 {
                layer_surface.destroy();
            }
        }
        if let Some(surface) = self.state.surface.take() {
            surface.destroy();
        }
        if let Some(output) = self.state.output.take() {
            if output.version() >= 3 {
                output.release();
            }
        }
        if let Some(pointer) = self.state.pointer.take() {
            if pointer.version() >= 3 {
                pointer.release();
            }
        }
        if let Some(seat) = self.state.seat.take() {
            if seat.version() >= 5 {
                seat.release();
            }
        }
        let _ = self.connection.flush();
    }
}

fn bind<I>(
    globals: &GlobalList,
    qh: &QueueHandle<State>,
    name: &'static str,
    versions: std::ops::RangeInclusive<u32>,
) -> Result<I, Error>
where
    I: Proxy + 'static,
    State: Dispatch<I, ()> + 'static,
{
    globals
        .bind::<I, State, ()>(qh, versions, ())
        .map_err(|error| match error {
            BindError::NotPresent => Error::MissingGlobal(name),
            BindError::UnsupportedVersion => Error::Unsupported(name),
        })
}

fn protocol_anchor(anchors: Anchors) -> zwlr_layer_surface_v1::Anchor {
    zwlr_layer_surface_v1::Anchor::from_bits_retain(anchors.bits())
}

fn protocol_keyboard(
    interactivity: KeyboardInteractivity,
) -> zwlr_layer_surface_v1::KeyboardInteractivity {
    match interactivity {
        KeyboardInteractivity::None => zwlr_layer_surface_v1::KeyboardInteractivity::None,
        KeyboardInteractivity::Exclusive => zwlr_layer_surface_v1::KeyboardInteractivity::Exclusive,
    }
}

fn protocol_layer(layer: Layer) -> zwlr_layer_shell_v1::Layer {
    match layer {
        Layer::Background => zwlr_layer_shell_v1::Layer::Background,
        Layer::Bottom => zwlr_layer_shell_v1::Layer::Bottom,
        Layer::Top => zwlr_layer_shell_v1::Layer::Top,
        Layer::Overlay => zwlr_layer_shell_v1::Layer::Overlay,
    }
}

/// Mutable state dispatched by the overlay's event queue.
struct State {
    requested_size: (u32, u32),
    configured: Option<(u32, u32)>,
    closed: bool,
    compositor: wl_compositor::WlCompositor,
    shm: wl_shm::WlShm,
    surface: Option<wl_surface::WlSurface>,
    layer_surface: Option<zwlr_layer_surface_v1::ZwlrLayerSurfaceV1>,
    output: Option<wl_output::WlOutput>,
    region: Option<wl_region::WlRegion>,
    egl: Option<Egl>,
    /// The seat, when the compositor advertises one.
    seat: Option<wl_seat::WlSeat>,
    /// The seat's pointer, created once the seat reports the capability.
    pointer: Option<wl_pointer::WlPointer>,
    /// Pointer events accumulated for the caller, drained by `take_events`.
    events: Vec<PointerEvent>,
    /// The `wl_pointer` frame being filled.
    frame: PointerFrame,
    /// Output names by bind index, filled while selecting a named output.
    output_names: Vec<Option<String>>,
}

impl State {
    fn new(
        requested_size: (u32, u32),
        compositor: wl_compositor::WlCompositor,
        shm: wl_shm::WlShm,
    ) -> State {
        State {
            requested_size,
            configured: None,
            closed: false,
            compositor,
            shm,
            surface: None,
            layer_surface: None,
            output: None,
            region: None,
            egl: None,
            seat: None,
            pointer: None,
            events: Vec::new(),
            frame: PointerFrame::default(),
            output_names: Vec::new(),
        }
    }

    /// Bind every advertised `wl_output`, learn their names, and return the one
    /// matching `name`.
    fn select_output(
        &mut self,
        globals: &GlobalList,
        qh: &QueueHandle<State>,
        queue: &mut EventQueue<State>,
        name: &str,
    ) -> Result<wl_output::WlOutput, Error> {
        let global_names: Vec<u32> = globals.contents().with_list(|list| {
            list.iter()
                .filter(|global| global.interface == "wl_output")
                .map(|global| global.name)
                .collect()
        });
        if global_names.is_empty() {
            return Err(Error::UnknownOutput(name.to_string()));
        }
        self.output_names = vec![None; global_names.len()];
        let registry = globals.registry();
        let mut outputs = Vec::with_capacity(global_names.len());
        for (index, global_name) in global_names.iter().enumerate() {
            outputs.push(registry.bind::<wl_output::WlOutput, u32, State>(
                *global_name,
                4,
                qh,
                index as u32,
            ));
        }
        queue
            .roundtrip(self)
            .map_err(|error| Error::Wayland(error.to_string()))?;

        let selected = self
            .output_names
            .iter()
            .position(|candidate| candidate.as_deref() == Some(name))
            .ok_or_else(|| Error::UnknownOutput(name.to_string()))?;
        let output = outputs.swap_remove(selected);
        for other in outputs {
            other.release();
        }
        self.output_names.clear();
        Ok(output)
    }
}

impl Dispatch<wl_registry::WlRegistry, GlobalListContents> for State {
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

impl Dispatch<wl_compositor::WlCompositor, ()> for State {
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

delegate_noop!(State: ignore wl_shm::WlShm);
delegate_noop!(State: ignore zwlr_layer_shell_v1::ZwlrLayerShellV1);
delegate_noop!(State: ignore wl_region::WlRegion);
delegate_noop!(State: ignore wl_surface::WlSurface);

impl Dispatch<wl_seat::WlSeat, ()> for State {
    fn event(
        state: &mut Self,
        seat: &wl_seat::WlSeat,
        event: wl_seat::Event,
        _: &(),
        _: &Connection,
        qh: &QueueHandle<Self>,
    ) {
        let wl_seat::Event::Capabilities { capabilities } = event else {
            return;
        };
        // An unknown-but-reserved capability value still means "no pointer" here.
        let pointer = matches!(capabilities, WEnum::Value(bits) if bits.contains(wl_seat::Capability::Pointer));
        match (pointer, state.pointer.is_some()) {
            (true, false) => {
                state.pointer = Some(seat.get_pointer(qh, ()));
            }
            (false, true) => {
                // The protocol requires destroying the pointer once the seat
                // loses the capability; further events on it are invalid.
                if let Some(pointer) = state.pointer.take() {
                    if pointer.version() >= 3 {
                        pointer.release();
                    }
                }
                state.frame.flush(&mut state.events);
            }
            _ => {}
        }
    }
}

/// Deliver the accumulated frame at once for a `wl_pointer` older than version
/// 5, which never sends the frame event.
fn flush_without_frame(state: &mut State, pointer: &wl_pointer::WlPointer) {
    if pointer.version() < 5 {
        state.frame.flush(&mut state.events);
    }
}

impl Dispatch<wl_pointer::WlPointer, ()> for State {
    fn event(
        state: &mut Self,
        pointer: &wl_pointer::WlPointer,
        event: wl_pointer::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            wl_pointer::Event::Enter {
                surface_x,
                surface_y,
                ..
            } => {
                // `surface_x`/`surface_y` are the protocol's `wl_fixed_t`
                // (/ 256) already converted to `f64` by wayland-scanner.
                state.frame.entered(surface_x, surface_y);
            }
            wl_pointer::Event::Leave { .. } => state.frame.left(),
            wl_pointer::Event::Motion {
                surface_x,
                surface_y,
                ..
            } => {
                state.frame.moved(surface_x, surface_y);
            }
            wl_pointer::Event::Button {
                button,
                state: button_state,
                ..
            } => {
                // The protocol defines only pressed and released; an unknown
                // value is skipped rather than treated as either.
                let pressed = match button_state {
                    WEnum::Value(wl_pointer::ButtonState::Pressed) => Some(true),
                    WEnum::Value(wl_pointer::ButtonState::Released) => Some(false),
                    _ => None,
                };
                if let Some(pressed) = pressed {
                    state.frame.button(button, pressed);
                }
            }
            wl_pointer::Event::Axis { axis, value, .. } => state.frame.axis(axis, value),
            wl_pointer::Event::AxisValue120 { axis, value120 } => {
                state.frame.axis_value120(axis, value120);
            }
            wl_pointer::Event::Frame => {
                state.frame.flush(&mut state.events);
                return;
            }
            _ => return,
        }
        flush_without_frame(state, pointer);
    }
}

impl Dispatch<wl_output::WlOutput, u32> for State {
    fn event(
        state: &mut Self,
        _: &wl_output::WlOutput,
        event: wl_output::Event,
        index: &u32,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        if let wl_output::Event::Name { name } = event {
            if let Some(slot) = state.output_names.get_mut(*index as usize) {
                *slot = Some(name);
            }
        }
    }
}

impl Dispatch<zwlr_layer_surface_v1::ZwlrLayerSurfaceV1, ()> for State {
    fn event(
        state: &mut Self,
        layer_surface: &zwlr_layer_surface_v1::ZwlrLayerSurfaceV1,
        event: zwlr_layer_surface_v1::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            zwlr_layer_surface_v1::Event::Configure {
                serial,
                width,
                height,
            } => {
                layer_surface.ack_configure(serial);
                let size = resolve_configured_size(state.requested_size, (width, height));
                state.configured = Some(size);
                // No commit here: the layer surface has a buffer attached only
                // by EGL, and committing without one would unmap it. The ack
                // applies to the next commit, which EGL performs on swap.
                if let Some(egl) = state.egl.as_mut() {
                    let _ = egl.resize(size);
                }
            }
            zwlr_layer_surface_v1::Event::Closed => state.closed = true,
            _ => {}
        }
    }
}
