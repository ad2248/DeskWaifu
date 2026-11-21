use std::{
    error::Error,
    io,
    time::{Duration, Instant},
};

#[cfg(feature = "native")]
use std::path::{Path, PathBuf};

use smithay_client_toolkit::{
    compositor::{CompositorHandler, CompositorState, Region},
    delegate_compositor, delegate_layer, delegate_output, delegate_pointer, delegate_registry,
    delegate_seat, delegate_shm, delegate_xdg_shell, delegate_xdg_window,
    output::{OutputHandler, OutputState},
    registry::{ProvidesRegistryState, RegistryState},
    registry_handlers,
    seat::{
        Capability, SeatHandler, SeatState,
        pointer::{PointerEvent, PointerHandler},
    },
    shell::{
        WaylandSurface,
        wlr_layer::{
            Anchor, KeyboardInteractivity, Layer, LayerShell, LayerShellHandler, LayerSurface,
            LayerSurfaceConfigure,
        },
        xdg::{
            XdgShell,
            window::{Window, WindowConfigure, WindowDecorations, WindowHandler},
        },
    },
    shm::{Shm, ShmHandler, slot::SlotPool},
};
use wayland_client::{
    Connection, Dispatch, Proxy, QueueHandle, delegate_noop,
    globals::registry_queue_init,
    protocol::{wl_keyboard, wl_output, wl_pointer, wl_seat, wl_shm, wl_surface},
};
use wayland_protocols::{
    wp::fractional_scale::v1::client::{
        wp_fractional_scale_manager_v1::WpFractionalScaleManagerV1,
        wp_fractional_scale_v1::{self, WpFractionalScaleV1},
    },
    wp::viewporter::client::{wp_viewport::WpViewport, wp_viewporter::WpViewporter},
};

const OVERLAY_SIZE: Size = Size(320, 240);
const UNDERLAY_SIZE: Size = Size(800, 600);
const HELP: &str = "Usage: layer-shell-probe [--output NAME] [--underlay | --empty-input] [--integer-scale]\n\
    \x20       [--switch-output NAME --switch-after-ms N] [--hide-after-ms N] [--exit-after-ms N]\n\
    --underlay: run a separate fullscreen xdg-shell input witness instead of the overlay\n\
    --empty-input: keep drawing the overlay with an empty input region\n\
    Timers start after global discovery, before the initial surface commit.";

type Result<T> = std::result::Result<T, Box<dyn Error>>;
#[cfg(feature = "native")]
const MODEL_SIZE: Size = Size(640, 480);
use self::model_pixels::{model_region, paint_model};
mod model_pixels;
mod placement;
#[cfg(feature = "native")]
mod screen_edit;
use placement::Placement;
#[cfg(feature = "native")]
use placement::drag_threshold;
#[cfg(feature = "native")]
pub use placement::{Crop, Transform};
#[cfg(feature = "native")]
use screen_edit::{ModelDrag, Release, SaveOutcome, ScreenEdit};

#[cfg(feature = "native")]
use crate::worker::{
    Command, Control, DragOutput, GlobalGaze, MotionGroup, OutputSave, ScreenSave,
};

#[cfg(feature = "native")]
enum Edit {
    Connection { owner: u64, original: Transform },
    Screen(ScreenEdit<wl_pointer::WlPointer>),
}

#[cfg_attr(not(feature = "native"), allow(dead_code))]
enum SceneAction<'action> {
    #[cfg(feature = "native")]
    Skin,
    #[cfg(feature = "native")]
    Tap(f32, f32),
    Draw(f32, Option<(f32, f32)>),
    #[cfg(not(feature = "native"))]
    Phantom(std::marker::PhantomData<&'action ()>),
    #[cfg(feature = "native")]
    Load(
        std::path::PathBuf,
        &'action mut Vec<MotionGroup>,
        &'action mut Vec<String>,
    ),
    #[cfg(feature = "native")]
    Motion(String, u32),
    #[cfg(feature = "native")]
    Expression(String),
    #[cfg(feature = "native")]
    ToggleExpression {
        file: &'action Path,
        fade_seconds: f32,
        deactivate_after: Option<f32>,
        active: &'action mut bool,
    },
    #[cfg(feature = "native")]
    RetainActiveExpressions(&'action mut Vec<PathBuf>),
    #[cfg(feature = "native")]
    LipSync(Option<f32>),
    #[cfg(feature = "native")]
    Look(Option<(f32, f32)>),
}

type RenderModel<'model> = dyn FnMut(SceneAction<'_>, &mut [u8]) -> Result<()> + 'model;

struct ModelFrame<'model> {
    size: Size,
    pixels: Vec<u8>,
    render: Box<RenderModel<'model>>,
    last_frame: Option<Instant>,
    animated: bool,
    #[cfg(feature = "native")]
    active_expression_toggles: Vec<PathBuf>,
}

#[derive(Default)]
struct Options {
    output: Option<String>,
    switch_output: Option<String>,
    switch_after: Option<Duration>,
    hide_after: Option<Duration>,
    exit_after: Option<Duration>,
    empty_input: bool,
    integer_scale: bool,
    underlay: bool,
    #[cfg(feature = "native")]
    model: Option<std::path::PathBuf>,
    #[cfg(feature = "native")]
    motion: Option<(String, u32)>,
}

impl Options {
    fn parse() -> Result<Option<Self>> {
        let mut options = Self::default();
        let mut args = std::env::args().skip(1);
        while let Some(arg) = args.next() {
            let mut value = || {
                args.next()
                    .ok_or_else(|| format!("missing value for {arg}"))
            };
            let mut duration = || -> Result<Duration> {
                Ok(Duration::from_millis(value()?.parse::<u32>()? as u64))
            };
            match arg.as_str() {
                "-h" | "--help" => return Ok(None),
                "--output" => options.output = Some(value()?),
                "--switch-output" => options.switch_output = Some(value()?),
                "--switch-after-ms" => options.switch_after = Some(duration()?),
                "--hide-after-ms" => options.hide_after = Some(duration()?),
                "--exit-after-ms" => options.exit_after = Some(duration()?),
                "--empty-input" => options.empty_input = true,
                "--integer-scale" => options.integer_scale = true,
                "--underlay" => options.underlay = true,
                #[cfg(feature = "native")]
                "--model" => options.model = Some(value()?.into()),
                #[cfg(feature = "native")]
                "--motion" => {
                    let raw = value()?;
                    let (group, index) = raw
                        .rsplit_once(':')
                        .ok_or("expected --motion GROUP:INDEX")?;
                    options.motion = Some((group.to_owned(), index.parse()?));
                }
                #[cfg(not(feature = "native"))]
                "--model" | "--motion" => {
                    return Err("--model and --motion require the native feature".into());
                }
                _ => return Err(format!("unknown argument {arg}; see --help").into()),
            }
        }
        if options.switch_after.is_some() != options.switch_output.is_some() {
            return Err("--switch-output and --switch-after-ms must be used together".into());
        }
        if options.underlay
            && (options.empty_input
                || options.switch_after.is_some()
                || options.hide_after.is_some())
        {
            return Err(
                "--underlay is incompatible with overlay input, switch and hide options".into(),
            );
        }
        if matches!((options.switch_after, options.hide_after), (Some(switch), Some(hide)) if switch >= hide)
        {
            return Err("switch must be scheduled before hide".into());
        }
        #[cfg(feature = "native")]
        if (options.model.is_some() && options.underlay)
            || (options.motion.is_some() && options.model.is_none())
        {
            return Err("--model is overlay-only and --motion requires --model".into());
        }
        Ok(Some(options))
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Size(u32, u32);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Scale {
    Integer(u32),
    Fractional(u32),
}

impl Scale {
    fn pixels(self, size: Size) -> Result<Size> {
        let scale = match self {
            Self::Integer(value) => u128::from(value) * 120,
            Self::Fractional(value) => u128::from(value),
        };
        let round = |axis| (u128::from(axis) * scale + 60) / 120;
        let (width, height) = (round(size.0), round(size.1));
        // Limit the probe's wl_shm allocation and keep logical and pixel math bounded.
        if !(1..=4096).contains(&width)
            || !(1..=4096).contains(&height)
            || !(1..=4096).contains(&size.0)
            || !(1..=4096).contains(&size.1)
        {
            return Err(format!("unsupported buffer size {width}x{height} for {size:?}").into());
        }
        Ok(Size(width as u32, height as u32))
    }
}

#[derive(Clone, Copy, Debug)]
struct Rect(i32, i32, i32, i32);

// The same logical rectangles describe both the synthetic model and its input region.
fn model_rects(Size(w, h): Size) -> [Rect; 2] {
    let (w, h) = (w as i32, h as i32);
    [
        Rect(w / 4, h / 4, w / 2, h / 2),
        Rect(w / 8, h / 2, w / 8, h / 4),
    ]
}

fn paint(canvas: &mut [u8], logical: Size, pixels: Size, underlay: bool) {
    let [main, side] = model_rects(logical);
    let contains = |Rect(rx, ry, rw, rh), x, y| x >= rx && x < rx + rw && y >= ry && y < ry + rh;
    for (index, bytes) in canvas[..(pixels.0 * pixels.1 * 4) as usize]
        .chunks_exact_mut(4)
        .enumerate()
    {
        let x = (((index as u32 % pixels.0) * 2 + 1) * logical.0 / (pixels.0 * 2)) as i32;
        let y = (((index as u32 / pixels.0) * 2 + 1) * logical.1 / (pixels.1 * 2)) as i32;
        let argb = if underlay {
            if (x / 32 + y / 32) % 2 == 0 {
                0xff30_4058u32
            } else {
                0xff50_6078
            }
        } else if contains(main, x, y) {
            0xc024_9c60 // Premultiplied RGBA, with alpha 192.
        } else if contains(side, x, y) {
            0x8058_3810 // Premultiplied RGBA, with alpha 128.
        } else {
            0
        };
        bytes.copy_from_slice(&argb.to_ne_bytes());
    }
}

struct FractionalGlobals {
    scale: WpFractionalScaleManagerV1,
    viewporter: WpViewporter,
}

struct FractionalSurface {
    scale: WpFractionalScaleV1,
    viewport: WpViewport,
}

impl Drop for FractionalSurface {
    fn drop(&mut self) {
        self.scale.destroy();
        self.viewport.destroy();
    }
}

enum Role {
    Overlay(LayerSurface),
    Underlay(Window),
}

impl Role {
    fn wl_surface(&self) -> &wl_surface::WlSurface {
        match self {
            Self::Overlay(role) => role.wl_surface(),
            Self::Underlay(role) => role.wl_surface(),
        }
    }
}

struct Surface {
    // Drop extension objects before the surface role.
    fractional: Option<FractionalSurface>,
    role: Role,
    output: Option<wl_output::WlOutput>,
    generation: u32,
    logical: Option<Size>,
    integer_scale: u32,
    fractional_scale: Option<u32>,
    redraw: bool,
    frame_pending: bool,
    mapped: bool,
    keyboard: KeyboardInteractivity,
}

#[cfg(feature = "native")]
struct OutputTransfer {
    candidate: Surface,
    sample: DragOutput,
    drag: ModelDrag,
    deadline: Instant,
}

#[cfg(feature = "native")]
fn centered_transform(mut placement: Placement, Size(width, height): Size) -> Transform {
    let (model_width, model_height) = placement.size();
    placement.origin = (
        (f64::from(width) - model_width) / 2.0,
        (f64::from(height) - model_height) / 2.0,
    );
    Transform::capture(placement, None)
}

#[cfg(feature = "native")]
struct OutputSelection {
    id: u64,
    name: Option<String>,
    candidate: Option<Surface>,
    deadline: Instant,
    save: Option<OutputSave>,
}

#[cfg(feature = "native")]
#[derive(Default)]
struct Gaze {
    target: Option<(f64, f64)>,
    next: Option<Instant>,
}

#[cfg(feature = "native")]
impl Gaze {
    fn sample(
        &mut self,
        provider: Option<&mut GlobalGaze>,
        output: Option<&(String, Size)>,
        now: Instant,
    ) -> bool {
        let next_target = if let (Some(provider), Some((name, Size(w, h)))) = (provider, output) {
            if self.next.is_some_and(|next| now < next) {
                return false;
            }
            self.next = Some(now + Duration::from_millis(16));
            provider(name, (*w, *h)).filter(|(x, y)| x.is_finite() && y.is_finite())
        } else {
            self.next = None;
            None
        };
        let changed = self.target != next_target;
        self.target = next_target;
        changed
    }
}

struct SeatDevices {
    seat: wl_seat::WlSeat,
    pointer: Option<wl_pointer::WlPointer>,
    keyboard: Option<wl_keyboard::WlKeyboard>,
}

struct Probe {
    options: Options,
    registry: RegistryState,
    outputs: OutputState,
    seats: SeatState,
    devices: Vec<SeatDevices>,
    compositor: CompositorState,
    layer_shell: Option<LayerShell>,
    xdg_shell: Option<XdgShell>,
    shm: Shm,
    pool: SlotPool,
    fractional: Option<FractionalGlobals>,
    surface: Option<Surface>,
    generation: u32,
    hidden: bool,
    closed: bool,
    lost_output: Option<wl_output::WlOutput>,
    started: Instant,
    animated: bool,
    interactive: bool,
    opacity: f64,
    hover_opacity: Option<f64>,
    hovered: bool,
    cursor: Option<(f64, f64)>,
    look_radius: Option<f64>,
    #[cfg(feature = "native")]
    gaze: Gaze,
    crop: Option<placement::Crop>,
    #[cfg(feature = "native")]
    edit: Option<Edit>,
    #[cfg(feature = "native")]
    next_screen_save: u64,
    #[cfg(feature = "native")]
    released_model_drag: Option<ModelDrag>,
    #[cfg(feature = "native")]
    output_transfer: Option<OutputTransfer>,
    #[cfg(feature = "native")]
    output_selection: Option<OutputSelection>,
    #[cfg(feature = "native")]
    startup_output_save: Option<OutputSave>,
    #[cfg(feature = "native")]
    focused_output: Option<String>,
    #[cfg(feature = "native")]
    resized: bool,
    #[cfg(feature = "native")]
    cycle_model: bool,
    #[cfg(feature = "native")]
    cycle_press: Option<wl_pointer::WlPointer>,
    #[cfg(feature = "native")]
    switch_skin: bool,
    #[cfg(feature = "native")]
    tap: Option<(f32, f32)>,
    #[cfg(feature = "native")]
    wheel_change: Option<(Instant, Transform)>,
    placement: Placement,
    #[cfg(feature = "native")]
    drag: Option<(wl_pointer::WlPointer, (f64, f64))>,
}

pub fn run() -> Result<()> {
    let Some(options) = Options::parse()? else {
        println!("{HELP}");
        #[cfg(feature = "native")]
        println!("    --model MODEL.model3.json [--motion GROUP:INDEX]");
        return Ok(());
    };
    let conn = Connection::connect_to_env()
        .map_err(|error| format!("Wayland connection unavailable: {error}"))?;
    #[cfg(feature = "native")]
    if let Some(path) = options.model.as_deref() {
        let runtime = cubism_bridge::Runtime::new(MODEL_SIZE.0, MODEL_SIZE.1)?;
        let mut model = runtime.load(path)?;
        if let Some((group, index)) = &options.motion {
            model.play_motion(group, *index)?;
        }
        let frame = native_frame(&runtime, Some(model), options.motion.is_some(), None);
        return run_session(conn, options, Some(frame), None);
    }
    run_session(
        conn,
        options,
        None,
        #[cfg(feature = "native")]
        None,
    )
}

#[cfg(feature = "native")]
fn native_frame<'a>(
    runtime: &'a cubism_bridge::Runtime,
    mut model: Option<cubism_bridge::Model<'a>>,
    animated: bool,
    expression_timeout: Option<f32>,
) -> ModelFrame<'a> {
    let mut external_look = None;
    ModelFrame {
        size: MODEL_SIZE,
        pixels: vec![0; cubism_bridge::rgba_len(MODEL_SIZE.0, MODEL_SIZE.1).unwrap()],
        render: Box::new(move |action, pixels| {
            match action {
                SceneAction::Load(path, motions, expressions) => {
                    let mut candidate = runtime.load(&path)?;
                    candidate.set_expression_timeout(expression_timeout)?;
                    candidate.render(0.0, pixels)?;
                    *motions = candidate
                        .motion_groups()?
                        .into_iter()
                        .map(|(name, count)| MotionGroup { name, count })
                        .collect();
                    *expressions = candidate.expressions()?;
                    model = Some(candidate);
                    external_look = None;
                }
                SceneAction::Motion(group, index) => model
                    .as_mut()
                    .ok_or("no model is loaded")?
                    .play_motion(&group, index)?,
                SceneAction::Expression(name) => model
                    .as_mut()
                    .ok_or("no model is loaded")?
                    .play_expression(&name)?,
                SceneAction::ToggleExpression {
                    file,
                    fade_seconds,
                    deactivate_after,
                    active,
                } => {
                    *active = model
                        .as_mut()
                        .ok_or("no model is loaded")?
                        .toggle_expression(file, fade_seconds, deactivate_after)?;
                }
                SceneAction::RetainActiveExpressions(files) => {
                    let model = model.as_ref().ok_or("no model is loaded")?;
                    retain_active_expressions(files, |file| model.expression_toggle_active(file))?;
                }
                SceneAction::LipSync(opening) => model
                    .as_mut()
                    .ok_or("no model is loaded")?
                    .set_lip_sync(opening)?,
                SceneAction::Look(target) => {
                    model
                        .as_mut()
                        .ok_or("no model is loaded")?
                        .set_look(target)?;
                    external_look = target;
                }
                SceneAction::Skin => model.as_mut().ok_or("no model is loaded")?.switch_skin()?,
                SceneAction::Tap(x, y) => {
                    model.as_mut().ok_or("no model is loaded")?.tap(x, y)?;
                }
                SceneAction::Draw(dt, local_look) => {
                    let model = model.as_mut().ok_or("no model is loaded")?;
                    model.set_look(external_look.or(local_look))?;
                    model.render(dt, pixels)?;
                }
            }
            Ok(())
        }),
        last_frame: None,
        animated,
        active_expression_toggles: Vec::new(),
    }
}

#[cfg(feature = "native")]
fn retain_active_expressions<E>(
    files: &mut Vec<PathBuf>,
    mut is_active: impl FnMut(&Path) -> std::result::Result<bool, E>,
) -> std::result::Result<(), E> {
    // Preserve the prior snapshot if native status cannot be read for any file.
    let mut active = Vec::new();
    for file in files.iter() {
        if is_active(file)? {
            active.push(file.clone());
        }
    }
    *files = active;
    Ok(())
}

#[cfg(feature = "native")]
pub(crate) fn run_managed(control: &mut Control) -> Result<()> {
    let conn = Connection::connect_to_env()
        .map_err(|error| format!("Wayland connection unavailable: {error}"))?;
    use cubism_bridge::{DeviceSelector, Runtime};
    let devices = Runtime::devices().unwrap_or_default();
    control.update(|status| status.gpu_devices = devices.into_iter().map(Into::into).collect());
    let runtime = match control.requested_gpu.as_str() {
        "auto" => Runtime::new(MODEL_SIZE.0, MODEL_SIZE.1),
        node if node.starts_with("/dev/dri/") => Runtime::new_on_device(
            MODEL_SIZE.0,
            MODEL_SIZE.1,
            DeviceSelector::RenderNode(std::path::Path::new(node)),
        ),
        pci => Runtime::new_on_device(MODEL_SIZE.0, MODEL_SIZE.1, DeviceSelector::Pci(pci)),
    }
    .map_err(|error| format!("EGL GPU {}: {error}", control.requested_gpu))?;
    control.update(|status| status.active_gpu = runtime.active_device().map(Into::into));
    run_session(
        conn,
        Options {
            output: control.startup_output.clone(),
            ..Options::default()
        },
        Some(native_frame(
            &runtime,
            None,
            false,
            control.expression_timeout,
        )),
        Some(control),
    )
}

fn run_session(
    conn: Connection,
    options: Options,
    mut frame: Option<ModelFrame<'_>>,
    #[cfg(feature = "native")] mut control: Option<&mut Control>,
) -> Result<()> {
    let (globals, mut queue) = registry_queue_init(&conn)?;
    let qh = queue.handle();
    let compositor = CompositorState::bind(&globals, &qh)?;
    let shm = Shm::bind(&globals, &qh)?;
    let (layer_shell, xdg_shell) = if options.underlay {
        (None, Some(XdgShell::bind(&globals, &qh)?))
    } else {
        let layer = LayerShell::bind(&globals, &qh)
            .map_err(|e| format!("zwlr_layer_shell_v1 unavailable: {e}"))?;
        (Some(layer), None)
    };
    let fractional = if options.integer_scale {
        None
    } else {
        globals
            .bind(&qh, 1..=1, ())
            .ok()
            .zip(globals.bind(&qh, 1..=1, ()).ok())
            .map(|(scale, viewporter)| FractionalGlobals { scale, viewporter })
    };
    println!(
        "scale_protocols={}",
        if fractional.is_some() {
            "fractional-scale+viewporter"
        } else {
            "integer"
        }
    );
    #[cfg(feature = "native")]
    let interactive = control.is_some();
    #[cfg(not(feature = "native"))]
    let interactive = false;
    let mut probe = Probe {
        options,
        registry: RegistryState::new(&globals),
        outputs: OutputState::new(&globals, &qh),
        seats: SeatState::new(&globals, &qh),
        devices: Vec::new(),
        compositor,
        layer_shell,
        xdg_shell,
        pool: SlotPool::new(1, &shm)?,
        shm,
        fractional,
        surface: None,
        generation: 0,
        hidden: false,
        closed: false,
        lost_output: None,
        started: Instant::now(),
        animated: frame.as_ref().is_some_and(|frame| frame.animated),
        interactive,
        opacity: 1.0,
        hover_opacity: None,
        hovered: false,
        cursor: None,
        look_radius: None,
        #[cfg(feature = "native")]
        gaze: Gaze::default(),
        crop: None,
        #[cfg(feature = "native")]
        edit: None,
        #[cfg(feature = "native")]
        next_screen_save: 1,
        #[cfg(feature = "native")]
        released_model_drag: None,
        #[cfg(feature = "native")]
        output_transfer: None,
        #[cfg(feature = "native")]
        output_selection: None,
        #[cfg(feature = "native")]
        startup_output_save: None,
        #[cfg(feature = "native")]
        focused_output: None,
        #[cfg(feature = "native")]
        resized: false,
        #[cfg(feature = "native")]
        cycle_model: false,
        #[cfg(feature = "native")]
        cycle_press: None,
        #[cfg(feature = "native")]
        switch_skin: false,
        #[cfg(feature = "native")]
        tap: None,
        #[cfg(feature = "native")]
        wheel_change: None,
        placement: Placement::default(),
        #[cfg(feature = "native")]
        drag: None,
    };
    queue.roundtrip(&mut probe)?;
    probe.started = Instant::now();
    #[cfg(feature = "native")]
    let standalone = control.is_none();
    #[cfg(not(feature = "native"))]
    let standalone = true;
    if standalone {
        let output = probe
            .options
            .output
            .as_deref()
            .map(|name| probe.named_output(name))
            .transpose()?;
        probe.create_surface(&qh, output);
    }
    #[cfg(feature = "native")]
    if let Some(control) = &control {
        control.update(|status| {
            probe.opacity = status.opacity;
            probe.hover_opacity = status.hover_opacity;
            probe.look_radius = status.mouse_look_radius;
            probe.placement = status.transform.placement();
            probe.crop = status.transform.crop;
            status.state = "ready";
        });
    }
    #[cfg(feature = "native")]
    if control.is_some()
        && probe
            .options
            .output
            .as_deref()
            .is_some_and(|name| probe.named_output(name).is_err())
    {
        let fallback = probe
            .outputs
            .outputs()
            .find_map(|output| probe.outputs.info(&output)?.name);
        eprintln!(
            "terminal startup output {:?} is unavailable; using {:?}",
            probe.options.output, fallback
        );
        probe.options.output.clone_from(&fallback);
        probe.startup_output_save = fallback.map(|output| OutputSave {
            id: 0,
            output: Some(output),
            transform: Transform::capture(probe.placement, probe.crop),
        });
    }
    loop {
        queue.dispatch_pending(&mut probe)?;
        #[cfg(feature = "native")]
        let mut queue_busy = false;
        #[cfg(feature = "native")]
        let mut output_result = None;
        #[cfg(feature = "native")]
        if let Some(control) = control.as_deref_mut() {
            if !control.open()? {
                break;
            }
            let lost_output = probe.lost_output.is_some();
            if std::mem::take(&mut probe.resized) || lost_output {
                probe.output_transfer = None;
                probe.reset_gestures();
                match probe.edit.as_mut() {
                    // An outstanding save owns the frozen draft until the host replies.
                    Some(Edit::Screen(edit)) if edit.pending.is_some() => {
                        edit.cancel_on_failure = true;
                    }
                    Some(Edit::Screen(_)) => probe.finish_screen_edit(false)?,
                    Some(Edit::Connection { owner, .. }) if lost_output => {
                        let owner = *owner;
                        probe.end_edit(owner, false, control)?;
                    }
                    _ => {}
                }
            }
            if let Some((id, error)) = control.take_screen_save_result() {
                if let Some(Edit::Screen(edit)) = probe.edit.as_mut() {
                    match edit.finish_save(id, error) {
                        SaveOutcome::Applied => probe.finish_screen_edit(true)?,
                        SaveOutcome::Canceled => probe.finish_screen_edit(false)?,
                        SaveOutcome::Retry => probe.redraw(),
                        SaveOutcome::Stale => {}
                    }
                }
            }
            let scene = frame.as_mut().expect("managed scene");
            output_result = probe.advance_output_selection(control);
            if output_result
                .as_ref()
                .is_some_and(|(_, error)| error.is_none())
            {
                scene.last_frame = None;
            }
            let pointer_session = control.session();
            for count in 1..=16 {
                let Some(pending) = control.next() else {
                    break;
                };
                let result = if !pending.is_current(control.session()) {
                    Err("model session changed before the command could run".into())
                } else if let Command::Output(output) = pending.command {
                    match probe.stage_output_selection(&qh, pending.id, output) {
                        Ok(()) => {
                            queue_busy = count == 16;
                            continue;
                        }
                        Err(error) => Err(error),
                    }
                } else {
                    probe.apply_command(&qh, scene, control, pending.command)
                };
                control.complete(pending.id, result.err().map(|error| error.to_string()));
                queue_busy = count == 16;
            }
            let tap = probe.tap.take();
            let skin = std::mem::take(&mut probe.switch_skin);
            if pointer_session == control.session()
                && probe.edit.is_none()
                && probe.surface.as_ref().is_some_and(|surface| surface.mapped)
            {
                let scene = frame.as_mut().expect("managed scene");
                if let Some((x, y)) = tap {
                    if let Err(error) = (scene.render)(SceneAction::Tap(x, y), &mut scene.pixels) {
                        eprintln!("terminal model tap: {error}");
                    } else {
                        probe.animated = true;
                        scene.last_frame = None;
                        if let Some(surface) = &mut probe.surface {
                            surface.redraw = true;
                        }
                    }
                }
                if skin {
                    if let Err(error) = probe.apply_command(&qh, scene, control, Command::Skin) {
                        eprintln!("terminal model skin: {error}");
                    }
                }
            }
            if std::mem::take(&mut probe.cycle_model) && probe.edit.is_none() {
                let result = (|| -> Result<()> {
                    let models = (control.catalog)()?;
                    if !models.is_empty() {
                        let current = control.model_path();
                        let index = models
                            .iter()
                            .position(|path| Some(path) == current.as_ref());
                        let next = index.map_or(0, |index| (index + 1) % models.len());
                        if current.as_ref() != Some(&models[next]) {
                            probe.apply_command(
                                &qh,
                                frame.as_mut().expect("managed scene"),
                                control,
                                Command::Load(models[next].clone()),
                            )?;
                        }
                    }
                    Ok(())
                })();
                if let Err(error) = result {
                    eprintln!("terminal cycle model: {error}");
                }
            }
            let scene = frame.as_mut().expect("managed scene");
            if probe.finish_output_transfer() {
                scene.last_frame = None;
            }
            if let Some(drag) = probe.released_model_drag.take() {
                probe.start_output_transfer(&qh, control, drag);
            }
            if !queue_busy {
                if let Some(owner) = control.take_canceled_edit() {
                    let _ = probe.end_edit(owner, false, control);
                }
            }
        }
        if probe.closed || !probe.tick(&qh)? {
            break;
        }
        #[cfg(feature = "native")]
        if let Some(control) = &mut control {
            probe.sample_gaze(control.global_gaze.as_mut());
        }
        probe.draw(&qh, frame.as_mut())?;
        let writable = match queue.flush() {
            Ok(()) => false,
            Err(wayland_client::backend::WaylandError::Io(error))
                if error.kind() == io::ErrorKind::WouldBlock =>
            {
                true
            }
            Err(error) => return Err(error.into()),
        };
        #[cfg(feature = "native")]
        if let Some(control) = &control {
            let visible =
                !probe.hidden && probe.surface.as_ref().is_some_and(|surface| surface.mapped);
            let output = probe
                .surface
                .as_ref()
                .and_then(|surface| surface.output.as_ref())
                .map(|output| probe.output_name(output));
            let mut outputs: Vec<_> = probe
                .outputs
                .outputs()
                .filter_map(|output| probe.outputs.info(&output)?.name)
                .collect();
            outputs.sort_unstable();
            control.update(|status| {
                status.visible = visible;
                status.transform = Transform::capture(probe.placement, probe.crop);
                if let Some(frame) = &frame {
                    status
                        .active_expression_toggles
                        .clone_from(&frame.active_expression_toggles);
                }
                status.wheel_change = probe.wheel_change;
                status.crop = probe.crop;
                if let Some(Edit::Screen(edit)) = &probe.edit {
                    status.screen_editing = true;
                    status.screen_save = edit.pending;
                    status.screen_save_error.clone_from(&edit.save_error);
                } else {
                    status.screen_editing = false;
                    status.screen_save = None;
                    status.screen_save_error = None;
                }
                status.output = output;
                status.selected_output.clone_from(&probe.options.output);
                status.output_save = probe.startup_output_save.clone().or_else(|| {
                    probe
                        .output_selection
                        .as_ref()
                        .and_then(|selection| selection.save.clone())
                });
                if let Some((id, error)) = output_result {
                    status.complete(id, error);
                }
                status.outputs = outputs;
            });
        }
        #[cfg(feature = "native")]
        if queue_busy {
            continue;
        }
        let Some(read_guard) = queue.prepare_read() else {
            continue;
        };
        let flags = if writable {
            rustix::event::PollFlags::IN | rustix::event::PollFlags::OUT
        } else {
            rustix::event::PollFlags::IN
        };
        let wayland_fd = rustix::event::PollFd::from_borrowed_fd(read_guard.connection_fd(), flags);
        #[cfg(feature = "native")]
        let wake = control
            .as_ref()
            .map(|control| rustix::event::PollFd::new(&control.wake, rustix::event::PollFlags::IN));
        let mut fds = [wayland_fd];
        #[cfg(feature = "native")]
        let mut fds_with_wake;
        #[cfg(feature = "native")]
        let fds: &mut [_] = if let Some(wake) = wake {
            fds_with_wake = [fds[0].clone(), wake];
            &mut fds_with_wake
        } else {
            &mut fds
        };
        #[cfg(not(feature = "native"))]
        let fds: &mut [_] = &mut fds;
        let timeout = probe.next_timeout();
        match rustix::event::poll(fds, timeout.as_ref()) {
            Ok(_) => {}
            Err(rustix::io::Errno::INTR) => continue,
            Err(error) => return Err(error.into()),
        }
        if fds[0].revents().intersects(
            rustix::event::PollFlags::IN
                | rustix::event::PollFlags::HUP
                | rustix::event::PollFlags::ERR,
        ) {
            match read_guard.read() {
                Ok(_) => {}
                Err(wayland_client::backend::WaylandError::Io(error))
                    if error.kind() == io::ErrorKind::WouldBlock => {}
                Err(error) => return Err(error.into()),
            }
        }
    }
    probe.surface.take();
    conn.flush()?;
    Ok(())
}

impl Probe {
    #[cfg(feature = "native")]
    fn apply_command(
        &mut self,
        qh: &QueueHandle<Self>,
        frame: &mut ModelFrame<'_>,
        control: &Control,
        command: Command,
    ) -> Result<()> {
        if self.output_selection.is_some()
            && matches!(
                command,
                Command::Show
                    | Command::Hide
                    | Command::Load(_)
                    | Command::FocusedOutput
                    | Command::Crop(_)
                    | Command::Transform(_)
                    | Command::BeginTransform { .. }
            )
        {
            return Err("wait for the output selection to finish".into());
        }
        if self.edit.is_some()
            && matches!(
                command,
                Command::Load(_)
                    | Command::Output(_)
                    | Command::FocusedOutput
                    | Command::Crop(_)
                    | Command::Transform(_)
            )
        {
            return Err("finish the active transform edit first".into());
        }
        match command {
            Command::Load(path) => {
                // The old native model survives if the staged load fails.
                let mut motions = Vec::new();
                let mut expressions = Vec::new();
                (frame.render)(
                    SceneAction::Load(path.clone(), &mut motions, &mut expressions),
                    &mut frame.pixels,
                )?;
                frame.last_frame = None;
                frame.active_expression_toggles.clear();
                self.reset_gestures();
                self.animated = control.animate;
                if !self.hidden {
                    if self.surface.is_none() {
                        self.create_surface(qh, self.preferred_output());
                    }
                    if let Some(surface) = &mut self.surface {
                        surface.redraw = true;
                    }
                }
                control.update(|status| {
                    status.model = Some(path);
                    status.motions = motions;
                    status.expressions = expressions;
                    status.active_expression_toggles.clear();
                    status.session += 1;
                });
            }
            Command::Show => {
                if control.status_has_no_model() {
                    return Err("no model is loaded".into());
                }
                self.hidden = false;
                frame.last_frame = None;
                if self.surface.is_none() {
                    self.create_surface(qh, self.preferred_output());
                }
                self.redraw();
            }
            Command::Hide => {
                if self.screen_editing() {
                    self.hidden = true;
                    self.hovered = false;
                    self.cursor = None;
                    self.reset_gestures();
                    self.redraw();
                } else {
                    self.hide_surface()?;
                }
                control.update(|status| status.visible = false);
            }
            Command::BeginTransform { owner } => {
                if self.edit.is_some() {
                    return Err("a transform edit is already active".into());
                }
                if control.status_has_no_model() {
                    return Err("no model is loaded".into());
                }
                self.edit = Some(Edit::Connection {
                    owner,
                    original: Transform::capture(self.placement, self.crop),
                });
                self.reset_gestures();
                control.update(|status| status.edit_owner = Some(owner));
            }
            Command::UpdateTransform { owner, transform } => {
                if !matches!(&self.edit, Some(Edit::Connection { owner: active, .. }) if *active == owner)
                {
                    return Err("transform edit is not owned by this connection".into());
                }
                self.set_transform(transform, control);
            }
            Command::EndTransform { owner, apply } => self.end_edit(owner, apply, control)?,
            Command::Transform(transform) => {
                self.wheel_change = None;
                self.set_transform(transform, control);
            }
            Command::Crop(crop) => {
                self.wheel_change = None;
                self.crop = crop;
                if let Some(surface) = &mut self.surface {
                    surface.redraw = true;
                }
                control.update(|status| status.crop = crop);
            }
            Command::HoverOpacity(opacity) => {
                if opacity.is_some_and(|value| !(0.0..=1.0).contains(&value)) {
                    return Err("hover opacity must be between 0 and 1".into());
                }
                self.hover_opacity = opacity;
                if let Some(surface) = &mut self.surface {
                    surface.redraw = true;
                }
                control.update(|status| status.hover_opacity = opacity);
            }
            Command::Opacity(opacity) => {
                if !(0.0..=1.0).contains(&opacity) {
                    return Err("opacity must be between 0 and 1".into());
                }
                self.opacity = opacity;
                if let Some(surface) = &mut self.surface {
                    surface.redraw = true;
                }
                control.update(|status| status.opacity = opacity);
            }
            Command::FocusedOutput => {
                let name = (control.focused_output)()?;
                let output = self.named_output(&name)?;
                self.focused_output = Some(name);
                if self
                    .surface
                    .as_ref()
                    .is_some_and(|surface| surface.output.as_ref() != Some(&output))
                {
                    frame.last_frame = None;
                    self.create_surface(qh, Some(output));
                }
            }
            Command::Output(_) => unreachable!("explicit output handled by the queue"),
            Command::ToggleExpression {
                file,
                fade_seconds,
                deactivate_after,
                ..
            } => {
                let mut active = false;
                (frame.render)(
                    SceneAction::ToggleExpression {
                        file: &file,
                        fade_seconds,
                        deactivate_after,
                        active: &mut active,
                    },
                    &mut frame.pixels,
                )?;
                let files = &mut frame.active_expression_toggles;
                match (files.binary_search(&file), active) {
                    (Ok(index), false) => {
                        files.remove(index);
                    }
                    (Err(index), true) => files.insert(index, file),
                    _ => {}
                }
                control.update(|status| status.active_expression_toggles.clone_from(files));
                self.animated = true;
                self.redraw();
            }
            command @ (Command::Motion { .. }
            | Command::Expression(_)
            | Command::LipSync(_)
            | Command::Look(_)
            | Command::Skin) => {
                let action = match command {
                    Command::Motion { group, index } => SceneAction::Motion(group, index),
                    Command::Expression(name) => SceneAction::Expression(name),
                    Command::LipSync(opening) => SceneAction::LipSync(opening),
                    Command::Look(target) => SceneAction::Look(target),
                    Command::Skin => SceneAction::Skin,
                    _ => unreachable!(),
                };
                (frame.render)(action, &mut frame.pixels)?;
                self.animated = true;
                frame.last_frame = None;
                if let Some(surface) = &mut self.surface {
                    surface.redraw = true;
                }
            }
        }
        Ok(())
    }

    #[cfg(feature = "native")]
    fn set_transform(&mut self, transform: Transform, control: &Control) {
        self.preview_transform(transform);
        self.reset_gestures();
        control.update(|status| {
            status.transform = transform;
            status.crop = transform.crop;
        });
    }

    #[cfg(feature = "native")]
    fn end_edit(&mut self, owner: u64, apply: bool, control: &Control) -> Result<()> {
        let Some(Edit::Connection { original, .. }) = self.edit.as_ref().filter(
            |edit| matches!(edit, Edit::Connection { owner: active, .. } if *active == owner),
        ) else {
            return Err("transform edit is not owned by this connection".into());
        };
        if !apply {
            self.set_transform(*original, control);
        }
        self.edit = None;
        control.update(|status| status.edit_owner = None);
        Ok(())
    }

    fn screen_editing(&self) -> bool {
        #[cfg(feature = "native")]
        return matches!(self.edit, Some(Edit::Screen(_)));
        #[cfg(not(feature = "native"))]
        false
    }

    #[cfg(feature = "native")]
    fn finish_screen_edit(&mut self, apply: bool) -> Result<()> {
        let Some(Edit::Screen(edit)) = self.edit.take() else {
            unreachable!("finishing requires a screen edit");
        };
        self.reset_gestures();
        if apply {
            self.wheel_change = None;
        } else {
            self.preview_transform(edit.original);
        }
        if self.hidden {
            self.hide_surface()?;
        } else {
            self.redraw();
        }
        Ok(())
    }

    fn reset_gestures(&mut self) {
        #[cfg(feature = "native")]
        {
            self.drag = None;
            self.tap = None;
            self.switch_skin = false;
            self.cycle_model = false;
            self.cycle_press = None;
            self.released_model_drag = None;
            if let Some(Edit::Screen(edit)) = &mut self.edit {
                edit.reset_input();
            }
        }
    }

    #[cfg(feature = "native")]
    fn preview_transform(&mut self, transform: Transform) {
        self.placement = transform.placement();
        self.crop = transform.crop;
        self.redraw();
    }

    #[cfg(feature = "native")]
    fn sample_gaze(&mut self, provider: Option<&mut GlobalGaze>) {
        let output = if provider.is_none()
            || self.hidden
            || self.edit.is_some()
            || self.look_radius.is_none()
        {
            None
        } else {
            self.surface.as_ref().and_then(|surface| {
                let size = surface.logical?;
                let name = self.outputs.info(surface.output.as_ref()?)?.name?;
                Some((name, size))
            })
        };
        if self.gaze.sample(provider, output.as_ref(), Instant::now()) {
            self.redraw();
        }
    }

    #[cfg(feature = "native")]
    fn redraw(&mut self) {
        if let Some(surface) = &mut self.surface {
            surface.redraw = true;
        }
    }

    #[cfg(feature = "native")]
    fn preferred_output(&self) -> Option<wl_output::WlOutput> {
        self.focused_output
            .as_deref()
            .and_then(|name| self.named_output(name).ok())
            .or_else(|| {
                self.options
                    .output
                    .as_deref()
                    .and_then(|name| self.named_output(name).ok())
            })
    }

    fn named_output(&self, name: &str) -> Result<wl_output::WlOutput> {
        self.outputs
            .outputs()
            .find(|output| {
                self.outputs
                    .info(output)
                    .is_some_and(|info| info.name.as_deref() == Some(name))
            })
            .ok_or_else(|| {
                format!("output {name:?} is unavailable; see output events above").into()
            })
    }

    fn output_name(&self, output: &wl_output::WlOutput) -> String {
        self.outputs
            .info(output)
            .and_then(|info| info.name)
            .unwrap_or_else(|| format!("{output:?}"))
    }

    fn create_surface(&mut self, qh: &QueueHandle<Self>, output: Option<wl_output::WlOutput>) {
        self.reset_gestures();
        self.hovered = false;
        self.cursor = None;
        #[cfg(feature = "native")]
        {
            self.gaze = Gaze::default();
        }
        self.surface = Some(self.build_surface(qh, output));
    }

    fn build_surface(
        &mut self,
        qh: &QueueHandle<Self>,
        output: Option<wl_output::WlOutput>,
    ) -> Surface {
        let raw = self.compositor.create_surface(qh);
        let role = if let Some(shell) = &self.layer_shell {
            let layer = shell.create_layer_surface(
                qh,
                raw,
                Layer::Overlay,
                Some("deskwaifu-layer-shell-probe"),
                output.as_ref(),
            );
            if self.interactive {
                layer.set_size(0, 0);
                layer.set_anchor(Anchor::TOP | Anchor::LEFT | Anchor::BOTTOM | Anchor::RIGHT);
            } else {
                layer.set_size(OVERLAY_SIZE.0, OVERLAY_SIZE.1);
                layer.set_anchor(Anchor::TOP | Anchor::LEFT);
            }
            layer.set_exclusive_zone(-1);
            layer.set_keyboard_interactivity(KeyboardInteractivity::None);
            Role::Overlay(layer)
        } else {
            let window = self
                .xdg_shell
                .as_ref()
                .expect("underlay has xdg-shell")
                .create_window(raw, WindowDecorations::None, qh);
            window.set_title("DeskWaifu P0 Input Witness");
            window.set_app_id("deskwaifu-layer-shell-underlay");
            window.set_fullscreen(output.as_ref());
            Role::Underlay(window)
        };
        self.generation += 1;
        let fractional = self.fractional.as_ref().map(|globals| FractionalSurface {
            scale: globals
                .scale
                .get_fractional_scale(role.wl_surface(), qh, self.generation),
            viewport: globals.viewporter.get_viewport(role.wl_surface(), qh, ()),
        });
        let integer_scale = output
            .as_ref()
            .and_then(|output| self.outputs.info(output))
            .map_or(1, |info| info.scale_factor.max(1) as u32);
        println!(
            "surface-created generation={} role={} output={}",
            self.generation,
            if self.options.underlay {
                "underlay"
            } else {
                "overlay"
            },
            output
                .as_ref()
                .map_or_else(|| "auto".to_owned(), |output| self.output_name(output))
        );
        role.wl_surface().commit();
        Surface {
            fractional,
            role,
            output,
            integer_scale,
            generation: self.generation,
            logical: None,
            fractional_scale: None,
            redraw: false,
            frame_pending: false,
            mapped: false,
            keyboard: KeyboardInteractivity::None,
        }
    }

    #[cfg(feature = "native")]
    fn stage_output_selection(
        &mut self,
        qh: &QueueHandle<Self>,
        id: u64,
        name: Option<String>,
    ) -> Result<()> {
        if self.edit.is_some() {
            return Err("finish the active transform edit first".into());
        }
        if self.output_selection.is_some()
            || self.startup_output_save.is_some()
            || self.lost_output.is_some()
            || self.closed
        {
            return Err("wait for the output selection to finish".into());
        }
        let target = name
            .as_deref()
            .map(|name| self.named_output(name))
            .transpose()?;
        self.reset_gestures();
        let candidate = self.build_surface(qh, target);
        self.output_selection = Some(OutputSelection {
            id,
            name,
            candidate: Some(candidate),
            deadline: Instant::now() + Duration::from_millis(500),
            save: None,
        });
        Ok(())
    }

    #[cfg(feature = "native")]
    fn advance_output_selection(&mut self, control: &Control) -> Option<(u64, Option<String>)> {
        let reply = control.take_output_save_result();
        if let Some((0, error)) = &reply {
            if self.startup_output_save.take().is_some() {
                if let Some(error) = error {
                    eprintln!("terminal startup output fallback save: {error}");
                }
            }
        }
        let selection = self.output_selection.as_mut()?;
        if let Some(save) = &selection.save {
            let (id, error) = reply.filter(|(id, _)| *id == save.id)?;
            let selection = self
                .output_selection
                .take()
                .expect("pending output selection");
            if error.is_some() {
                return Some((id, error));
            }
            let save = selection.save.expect("published output save");
            self.options.output = save.output;
            self.focused_output = None;
            if let Some(candidate) = selection.candidate {
                if self.surface.is_some() && !self.hidden && !self.closed {
                    self.lost_output = None;
                    self.surface = Some(candidate);
                    self.gaze = Gaze::default();
                    self.hovered = false;
                    self.cursor = None;
                }
                self.wheel_change = None;
                self.preview_transform(save.transform);
                return Some((id, None));
            }
            self.wheel_change = None;
            self.preview_transform(save.transform);
            return Some((
                id,
                Some("output disappeared while persistence was in progress".into()),
            ));
        }
        if selection.candidate.is_none()
            || self.closed
            || self.lost_output.is_some()
            || Instant::now() >= selection.deadline
        {
            let id = selection.id;
            self.output_selection = None;
            return Some((id, Some("destination output failed to configure".into())));
        }
        let size = selection
            .candidate
            .as_ref()
            .and_then(|candidate| candidate.logical)?;
        selection.save = Some(OutputSave {
            id: selection.id,
            output: selection.name.clone(),
            transform: centered_transform(self.placement, size),
        });
        None
    }

    #[cfg(feature = "native")]
    fn start_output_transfer(
        &mut self,
        qh: &QueueHandle<Self>,
        control: &Control,
        drag: ModelDrag,
    ) {
        if self.hidden
            || self.closed
            || self.lost_output.is_some()
            || self.output_transfer.is_some()
            || !matches!(&self.edit, Some(Edit::Screen(edit)) if edit.pending.is_none())
        {
            return;
        }
        let Some(source) = self
            .surface
            .as_ref()
            .filter(|surface| surface.mapped)
            .and_then(|surface| surface.output.as_ref())
        else {
            return;
        };
        let sample = match (control.drag_output)() {
            Ok(Some(sample)) => sample,
            Ok(None) => return,
            Err(error) => {
                eprintln!("terminal screen drag output: {error}");
                return;
            }
        };
        let Ok(target) = self.named_output(&sample.connector) else {
            return;
        };
        if source == &target {
            return;
        }
        self.reset_gestures();
        let candidate = self.build_surface(qh, Some(target));
        self.output_transfer = Some(OutputTransfer {
            candidate,
            sample,
            drag,
            deadline: Instant::now() + Duration::from_millis(500),
        });
    }

    #[cfg(feature = "native")]
    fn finish_output_transfer(&mut self) -> bool {
        let Some(transfer) = &self.output_transfer else {
            return false;
        };
        if self.closed || self.lost_output.is_some() || Instant::now() >= transfer.deadline {
            self.output_transfer = None;
            return false;
        }
        let Some(size) = transfer.candidate.logical else {
            return false;
        };
        let transfer = self.output_transfer.take().expect("candidate configured");
        let Some(draft) = transfer.drag.rebase(&transfer.sample, size) else {
            return false;
        };
        self.surface = Some(transfer.candidate);
        self.reset_gestures();
        self.hovered = false;
        self.cursor = None;
        self.preview_transform(draft);
        true
    }

    fn tick(&mut self, qh: &QueueHandle<Self>) -> Result<bool> {
        let elapsed = self.started.elapsed();
        if self.options.exit_after.is_some_and(|time| elapsed >= time) {
            return Ok(false);
        }
        if self.options.hide_after.is_some_and(|time| elapsed >= time) {
            self.options.hide_after = None;
            self.hide_surface()?;
        }
        if let Some(lost) = self.lost_output.take() {
            self.surface.take();
            if !self.hidden {
                let replacement = self.outputs.outputs().find(|output| output != &lost);
                self.create_surface(qh, replacement);
            }
        }
        if self
            .options
            .switch_after
            .is_some_and(|time| elapsed >= time)
        {
            self.options.switch_after = None;
            let name = self
                .options
                .switch_output
                .take()
                .expect("paired with switch timer");
            if !self.hidden {
                let next = self.named_output(&name)?;
                self.surface.take();
                self.create_surface(qh, Some(next));
            }
        }
        Ok(true)
    }

    fn hide_surface(&mut self) -> Result<()> {
        self.hidden = true;
        self.hovered = false;
        self.cursor = None;
        self.reset_gestures();
        if let Some(surface) = self.surface.take() {
            if surface.mapped {
                let empty = Region::new(&self.compositor)?;
                surface
                    .role
                    .wl_surface()
                    .set_input_region(Some(empty.wl_region()));
                surface.role.wl_surface().attach(None, 0, 0);
                surface.role.wl_surface().commit();
            }
            println!("hidden generation={}", surface.generation);
        }
        Ok(())
    }

    fn next_timeout(&self) -> Option<rustix::event::Timespec> {
        let deadline = [
            self.options.exit_after,
            self.options.switch_after,
            self.options.hide_after,
        ]
        .into_iter()
        .flatten()
        .min();
        let now = Instant::now();
        let wait =
            deadline.map(|deadline| deadline.saturating_sub(now.duration_since(self.started)));
        #[cfg(feature = "native")]
        let wait = wait
            .into_iter()
            .chain(
                self.output_transfer
                    .as_ref()
                    .map(|transfer| transfer.deadline.saturating_duration_since(now)),
            )
            .chain(
                self.gaze
                    .next
                    .map(|next| next.saturating_duration_since(now)),
            )
            .min();
        let wait = wait?;
        Some(rustix::event::Timespec {
            tv_sec: wait.as_secs() as _,
            tv_nsec: wait.subsec_nanos().into(),
        })
    }

    fn draw(&mut self, qh: &QueueHandle<Self>, frame: Option<&mut ModelFrame<'_>>) -> Result<()> {
        let screen_editing = self.screen_editing();
        let Some(surface) = self.surface.as_mut() else {
            return Ok(());
        };
        let Some(logical) = surface.logical.filter(|_| surface.redraw) else {
            return Ok(());
        };
        surface.redraw = false;
        let raw = surface.role.wl_surface();
        let integer = if raw.version() >= 3 {
            surface.integer_scale
        } else {
            1
        };
        let scale = surface
            .fractional_scale
            .map_or(Scale::Integer(integer), Scale::Fractional);
        let pixels = scale.pixels(logical)?;
        let (buffer, canvas) = self.pool.create_buffer(
            pixels.0 as i32,
            pixels.1 as i32,
            (pixels.0 * 4) as i32,
            wl_shm::Format::Argb8888,
        )?;
        let model_region = if self.hidden {
            canvas.fill(0);
            None
        } else if let Some(frame) = frame {
            let now = Instant::now();
            let dt = frame
                .last_frame
                .map_or(0.0, |last| (now - last).as_secs_f32().min(0.1));
            let cursor = self.cursor;
            #[cfg(feature = "native")]
            let cursor = self.gaze.target.or(cursor);
            let local_look = self
                .look_radius
                .and_then(|radius| cursor.map(|target| self.placement.look_at(target, radius)));
            #[cfg(feature = "native")]
            let local_look = if self.edit.is_some() {
                None
            } else {
                local_look
            };
            (frame.render)(SceneAction::Draw(dt, local_look), &mut frame.pixels)?;
            #[cfg(feature = "native")]
            if !frame.active_expression_toggles.is_empty() {
                (frame.render)(
                    SceneAction::RetainActiveExpressions(&mut frame.active_expression_toggles),
                    &mut frame.pixels,
                )?;
            }
            frame.last_frame = Some(now);
            let clip = self.crop.map(|crop| crop.clip(logical));
            let bounds = if self.interactive {
                Some(model_pixels::paint_placed(
                    canvas,
                    pixels,
                    logical,
                    frame,
                    self.placement,
                    clip.as_ref(),
                ))
            } else {
                paint_model(canvas, pixels, frame);
                None
            };
            let region = (!self.options.empty_input && !screen_editing).then(|| {
                let mut region = model_region(canvas, pixels, logical, bounds);
                if let Some((x, y)) = clip.as_ref() {
                    model_pixels::clip_region(&mut region, x, y);
                }
                region
            });
            let opacity = if self.hovered {
                self.hover_opacity.unwrap_or(self.opacity)
            } else {
                self.opacity
            };
            model_pixels::apply_opacity(canvas, opacity);
            region
        } else {
            paint(canvas, logical, pixels, self.options.underlay);
            None
        };
        #[cfg(feature = "native")]
        if let Some(Edit::Screen(edit)) = &self.edit {
            screen_edit::paint_frame(
                canvas,
                pixels,
                logical,
                self.crop.expect("screen crop"),
                edit.save_error.is_some(),
            );
        }
        if let Role::Overlay(layer) = &surface.role {
            let keyboard = if screen_editing {
                KeyboardInteractivity::Exclusive
            } else {
                KeyboardInteractivity::None
            };
            if surface.keyboard != keyboard {
                layer.set_keyboard_interactivity(keyboard);
                surface.keyboard = keyboard;
            }
        }
        if raw.version() >= 3 {
            raw.set_buffer_scale(match scale {
                Scale::Integer(integer) => integer as i32,
                Scale::Fractional(_) => 1,
            });
        }
        if let Some(fractional) = &surface.fractional {
            match scale {
                Scale::Fractional(_) => fractional
                    .viewport
                    .set_destination(logical.0 as i32, logical.1 as i32),
                Scale::Integer(_) => fractional.viewport.set_destination(-1, -1),
            }
        }
        let input = if self.options.underlay || screen_editing {
            raw.set_input_region(None);
            if screen_editing {
                "screen-edit"
            } else {
                "default"
            }
        } else {
            let region = Region::new(&self.compositor)?;
            if !self.options.empty_input {
                let fallback = model_rects(logical);
                for &Rect(x, y, w, h) in model_region.as_deref().unwrap_or(&fallback) {
                    region.add(x, y, w, h);
                }
            }
            raw.set_input_region(Some(region.wl_region()));
            if self.options.empty_input {
                "empty"
            } else {
                "model"
            }
        };
        buffer.attach_to(raw)?;
        if raw.version() >= 4 {
            raw.damage_buffer(0, 0, pixels.0 as i32, pixels.1 as i32);
        } else {
            raw.damage(0, 0, logical.0 as i32, logical.1 as i32);
        }
        if !surface.frame_pending {
            raw.frame(qh, raw.clone());
            surface.frame_pending = true;
        }
        raw.commit();
        let first_frame = !surface.mapped;
        surface.mapped = true;
        if first_frame || !self.animated {
            println!(
                "committed generation={} logical={}x{} buffer={}x{} scale={scale:?} input={input}",
                surface.generation, logical.0, logical.1, pixels.0, pixels.1
            );
        }
        Ok(())
    }
}

impl Probe {
    fn current(&mut self, raw: &wl_surface::WlSurface) -> Option<&mut Surface> {
        #[cfg(feature = "native")]
        if let Some(transfer) = &mut self.output_transfer {
            if transfer.candidate.role.wl_surface() == raw {
                return Some(&mut transfer.candidate);
            }
        }
        #[cfg(feature = "native")]
        if let Some(candidate) = self
            .output_selection
            .as_mut()
            .and_then(|selection| selection.candidate.as_mut())
        {
            if candidate.role.wl_surface() == raw {
                return Some(candidate);
            }
        }
        self.surface
            .as_mut()
            .filter(|surface| surface.role.wl_surface() == raw)
    }

    fn configure_surface(&mut self, raw: &wl_surface::WlSurface, serial: u32, size: Size) {
        #[cfg(feature = "native")]
        if self
            .surface
            .as_ref()
            .filter(|surface| surface.role.wl_surface() == raw)
            .and_then(|surface| surface.logical)
            .is_some_and(|old| old != size)
        {
            self.resized = true;
        }
        if let Some(surface) = self.current(raw) {
            println!(
                "configure generation={} serial={serial} logical={}x{}",
                surface.generation, size.0, size.1
            );
            surface.logical = Some(size);
            surface.redraw = true;
        }
    }

    fn log_output(&self, event: &str, output: &wl_output::WlOutput) {
        if let Some(info) = self.outputs.info(output) {
            println!(
                "output-{event} name={} integer-scale={} logical-size={:?}",
                self.output_name(output),
                info.scale_factor,
                info.logical_size
            );
        }
    }
}

impl CompositorHandler for Probe {
    fn scale_factor_changed(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        raw: &wl_surface::WlSurface,
        factor: i32,
    ) {
        if let Some(surface) = self.current(raw) {
            surface.integer_scale = factor.max(1) as u32;
            surface.redraw |= surface.fractional_scale.is_none();
        }
    }
    fn transform_changed(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        _: &wl_surface::WlSurface,
        _: wl_output::Transform,
    ) {
    }
    fn frame(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        raw: &wl_surface::WlSurface,
        time: u32,
    ) {
        let animated = self.animated;
        if let Some(surface) = self.current(raw) {
            surface.frame_pending = false;
            surface.redraw |= animated;
            if !animated {
                println!("frame generation={} time={time}", surface.generation);
            }
        }
    }
    fn surface_enter(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        raw: &wl_surface::WlSurface,
        output: &wl_output::WlOutput,
    ) {
        let name = self.output_name(output);
        if let Some(surface) = self.current(raw) {
            surface.output = Some(output.clone());
            println!(
                "surface-enter generation={} output={name}",
                surface.generation
            );
        }
    }
    fn surface_leave(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        raw: &wl_surface::WlSurface,
        output: &wl_output::WlOutput,
    ) {
        let name = self.output_name(output);
        if let Some(surface) = self.current(raw) {
            println!(
                "surface-leave generation={} output={name}",
                surface.generation
            );
        }
    }
}

impl OutputHandler for Probe {
    fn output_state(&mut self) -> &mut OutputState {
        &mut self.outputs
    }
    fn new_output(&mut self, _: &Connection, _: &QueueHandle<Self>, output: wl_output::WlOutput) {
        self.log_output("added", &output);
    }
    fn update_output(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        output: wl_output::WlOutput,
    ) {
        self.log_output("changed", &output);
    }
    fn output_destroyed(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        output: wl_output::WlOutput,
    ) {
        println!("output-removed {}", self.output_name(&output));
        #[cfg(feature = "native")]
        if self
            .output_transfer
            .as_ref()
            .is_some_and(|transfer| transfer.candidate.output.as_ref() == Some(&output))
        {
            self.output_transfer = None;
        }
        #[cfg(feature = "native")]
        if let Some(selection) = &mut self.output_selection {
            if selection
                .candidate
                .as_ref()
                .is_some_and(|candidate| candidate.output.as_ref() == Some(&output))
            {
                selection.candidate = None;
            }
        }
        if !self.options.underlay
            && self
                .surface
                .as_ref()
                .is_some_and(|surface| surface.output.as_ref() == Some(&output))
        {
            self.lost_output = Some(output);
        }
    }
}

impl LayerShellHandler for Probe {
    fn closed(&mut self, _: &Connection, _: &QueueHandle<Self>, layer: &LayerSurface) {
        #[cfg(feature = "native")]
        if self
            .output_transfer
            .as_ref()
            .is_some_and(|transfer| transfer.candidate.role.wl_surface() == layer.wl_surface())
        {
            self.output_transfer = None;
            return;
        }
        #[cfg(feature = "native")]
        if let Some(selection) = &mut self.output_selection {
            if selection
                .candidate
                .as_ref()
                .is_some_and(|candidate| candidate.role.wl_surface() == layer.wl_surface())
            {
                selection.candidate = None;
                return;
            }
        }
        if let Some(surface) = self.current(layer.wl_surface()) {
            println!("surface-closed generation={}", surface.generation);
            // A compositor may send closed before the registry's output-removal event.
            if let Some(output) = surface.output.clone() {
                self.lost_output = Some(output);
            } else {
                self.closed = true;
            }
        }
    }
    fn configure(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        layer: &LayerSurface,
        config: LayerSurfaceConfigure,
        serial: u32,
    ) {
        // SCTK acknowledges the serial before calling this handler.
        let (w, h) = config.new_size;
        self.configure_surface(
            layer.wl_surface(),
            serial,
            Size(
                if w == 0 { OVERLAY_SIZE.0 } else { w },
                if h == 0 { OVERLAY_SIZE.1 } else { h },
            ),
        );
    }
}

impl WindowHandler for Probe {
    fn request_close(&mut self, _: &Connection, _: &QueueHandle<Self>, window: &Window) {
        if self.current(window.wl_surface()).is_some() {
            self.closed = true;
        }
    }
    fn configure(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        window: &Window,
        config: WindowConfigure,
        serial: u32,
    ) {
        self.configure_surface(
            window.wl_surface(),
            serial,
            Size(
                config.new_size.0.map_or(UNDERLAY_SIZE.0, |w| w.get()),
                config.new_size.1.map_or(UNDERLAY_SIZE.1, |h| h.get()),
            ),
        );
    }
}

impl SeatDevices {
    fn release(&mut self, capability: Capability) {
        match capability {
            Capability::Pointer => {
                if let Some(pointer) = self.pointer.take().filter(|p| p.version() >= 3) {
                    pointer.release();
                }
            }
            Capability::Keyboard => {
                if let Some(keyboard) = self.keyboard.take().filter(|k| k.version() >= 3) {
                    keyboard.release();
                }
            }
            _ => {}
        }
    }
}

impl SeatHandler for Probe {
    fn seat_state(&mut self) -> &mut SeatState {
        &mut self.seats
    }
    fn new_seat(&mut self, _: &Connection, _: &QueueHandle<Self>, seat: wl_seat::WlSeat) {
        self.devices.push(SeatDevices {
            seat,
            pointer: None,
            keyboard: None,
        });
    }
    fn new_capability(
        &mut self,
        _: &Connection,
        qh: &QueueHandle<Self>,
        seat: wl_seat::WlSeat,
        capability: Capability,
    ) {
        let index = self
            .devices
            .iter()
            .position(|device| device.seat == seat)
            .unwrap_or_else(|| {
                self.devices.push(SeatDevices {
                    seat: seat.clone(),
                    pointer: None,
                    keyboard: None,
                });
                self.devices.len() - 1
            });
        let device = &mut self.devices[index];
        match capability {
            Capability::Pointer if device.pointer.is_none() => {
                match self.seats.get_pointer(qh, &seat) {
                    Ok(pointer) => device.pointer = Some(pointer),
                    Err(error) => eprintln!("could not bind seat pointer: {error}"),
                }
            }
            Capability::Keyboard if device.keyboard.is_none() => {
                device.keyboard = Some(seat.get_keyboard(qh, ()))
            }
            _ => {}
        }
    }
    fn remove_capability(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        seat: wl_seat::WlSeat,
        capability: Capability,
    ) {
        if let Some(device) = self.devices.iter_mut().find(|device| device.seat == seat) {
            #[cfg(feature = "native")]
            let pointer = (capability == Capability::Pointer)
                .then(|| device.pointer.clone())
                .flatten();
            device.release(capability);
            #[cfg(feature = "native")]
            if let Some(pointer) = pointer {
                self.forget_pointer(&pointer);
            }
        }
    }
    fn remove_seat(&mut self, _: &Connection, _: &QueueHandle<Self>, seat: wl_seat::WlSeat) {
        if let Some(index) = self.devices.iter().position(|device| device.seat == seat) {
            let mut device = self.devices.swap_remove(index);
            #[cfg(feature = "native")]
            if let Some(pointer) = &device.pointer {
                self.forget_pointer(pointer);
            }
            device.release(Capability::Pointer);
            device.release(Capability::Keyboard);
        }
    }
}

#[cfg(feature = "native")]
impl Probe {
    fn forget_pointer(&mut self, pointer: &wl_pointer::WlPointer) {
        if self
            .drag
            .as_ref()
            .is_some_and(|(owner, _)| owner == pointer)
        {
            self.drag = None;
        }
        if self.cycle_press.as_ref() == Some(pointer) {
            self.cycle_press = None;
        }
        if let Some(Edit::Screen(edit)) = &mut self.edit {
            edit.forget_pointer(pointer);
        }
    }

    fn interact(&mut self, pointer: &wl_pointer::WlPointer, event: &PointerEvent) {
        use smithay_client_toolkit::seat::pointer::PointerEventKind;
        const LEFT: u32 = 0x110;
        if self.resized
            || self.lost_output.is_some()
            || self.closed
            || self.output_transfer.is_some()
            || self.output_selection.is_some()
        {
            return;
        }
        let Some(viewport) = self
            .surface
            .as_ref()
            .filter(|surface| surface.mapped && surface.role.wl_surface() == &event.surface)
            .and_then(|surface| surface.logical)
        else {
            return;
        };
        let cursor = event.position;
        if !cursor.0.is_finite() || !cursor.1.is_finite() {
            return;
        }
        let draft = Transform::capture(self.placement, self.crop);
        if let Some(edit) = &mut self.edit {
            if let Edit::Screen(edit) = edit {
                match event.kind {
                    PointerEventKind::Press { button: LEFT, .. } => {
                        edit.press(pointer, cursor, draft, viewport)
                    }
                    PointerEventKind::Release {
                        button: LEFT, time, ..
                    } => {
                        let preview = edit.motion(pointer, cursor, viewport);
                        match edit.release(pointer, cursor, time, viewport) {
                            Release::Apply => {
                                let id = self.next_screen_save;
                                self.next_screen_save += 1;
                                edit.pending = Some(ScreenSave {
                                    id,
                                    transform: draft,
                                });
                                self.redraw();
                            }
                            release => {
                                if let Release::ModelDrag(drag) = release {
                                    self.released_model_drag = Some(drag);
                                }
                                if let Some(preview) = preview {
                                    self.preview_transform(preview);
                                }
                            }
                        }
                    }
                    PointerEventKind::Motion { .. } => {
                        if let Some(preview) = edit.motion(pointer, cursor, viewport) {
                            self.preview_transform(preview);
                        }
                    }
                    PointerEventKind::Leave { .. } => edit.forget_pointer(pointer),
                    _ => {}
                }
            }
            return;
        }
        if self.hidden {
            return;
        }
        let owns_drag = self
            .drag
            .as_ref()
            .is_some_and(|(owner, _)| owner == pointer);
        match event.kind {
            PointerEventKind::Press { button: LEFT, .. } if self.drag.is_none() => {
                self.drag = Some((pointer.clone(), cursor));
            }
            PointerEventKind::Motion { .. } | PointerEventKind::Release { button: LEFT, .. }
                if owns_drag =>
            {
                let (_, press) = self.drag.as_ref().expect("matched drag owner");
                let press = *press;
                let release = matches!(event.kind, PointerEventKind::Release { .. });
                if drag_threshold((cursor.0 - press.0, cursor.1 - press.1)) {
                    self.reset_gestures();
                    let mut edit = ScreenEdit::begin(draft, pointer.clone(), press, viewport);
                    let preview = edit
                        .motion(pointer, cursor, viewport)
                        .expect("drag crossed threshold");
                    if let PointerEventKind::Release { time, .. } = event.kind {
                        if let Release::ModelDrag(drag) =
                            edit.release(pointer, cursor, time, viewport)
                        {
                            self.released_model_drag = Some(drag);
                        }
                    }
                    self.edit = Some(Edit::Screen(edit));
                    self.preview_transform(preview);
                } else if release {
                    self.drag = None;
                    let (w, h) = self.placement.size();
                    let (x, y) = self.placement.origin;
                    let (u, v) = ((cursor.0 - x) / w, (cursor.1 - y) / h);
                    if (0.0..=1.0).contains(&u) && (0.0..=1.0).contains(&v) {
                        self.tap = Some((u as f32, v as f32));
                    }
                }
            }
            PointerEventKind::Leave { .. } => self.forget_pointer(pointer),
            PointerEventKind::Press { button: 0x111, .. } if self.drag.is_none() => {
                self.cycle_press = Some(pointer.clone());
            }
            PointerEventKind::Release { button: 0x111, .. }
                if self.cycle_press.as_ref() == Some(pointer) =>
            {
                self.cycle_press = None;
                self.cycle_model |= self.drag.is_none();
            }
            PointerEventKind::Press { button: 0x112, .. } if self.drag.is_none() => {
                self.switch_skin = true;
            }
            PointerEventKind::Axis { vertical, .. } if self.drag.is_none() => {
                let steps = if vertical.value120 != 0 {
                    f64::from(vertical.value120) / 120.0
                } else if vertical.discrete != 0 {
                    f64::from(vertical.discrete)
                } else {
                    vertical.absolute / 15.0
                };
                if steps.is_finite() && steps != 0.0 {
                    self.placement.scroll(cursor, -steps, viewport);
                    self.wheel_change = Some((
                        Instant::now(),
                        Transform::capture(self.placement, self.crop),
                    ));
                    self.redraw();
                }
            }
            _ => {}
        }
    }
}

impl PointerHandler for Probe {
    fn pointer_frame(
        &mut self,
        _: &Connection,
        _: &QueueHandle<Self>,
        _pointer: &wl_pointer::WlPointer,
        events: &[PointerEvent],
    ) {
        use smithay_client_toolkit::seat::pointer::PointerEventKind;
        for event in events {
            if self.interactive && self.current(&event.surface).is_some() {
                let cursor = match event.kind {
                    PointerEventKind::Enter { .. } | PointerEventKind::Motion { .. } => {
                        Some(Some(event.position))
                    }
                    PointerEventKind::Leave { .. } => Some(None),
                    _ => None,
                };
                if let Some(cursor) = cursor {
                    let cursor = cursor.filter(|(x, y)| x.is_finite() && y.is_finite());
                    let hovered = cursor.is_some();
                    let redraw = (self.hover_opacity.is_some() && hovered != self.hovered)
                        || (self.look_radius.is_some() && cursor != self.cursor);
                    self.hovered = hovered;
                    self.cursor = cursor;
                    if redraw {
                        self.current(&event.surface)
                            .expect("current surface")
                            .redraw = true;
                    }
                }
            }
            #[cfg(feature = "native")]
            if self.interactive {
                self.interact(_pointer, event);
            }
            if let Some(surface) = self.current(&event.surface) {
                let (x, y) = event.position;
                let event = match event.kind {
                    PointerEventKind::Enter { .. } => "enter",
                    PointerEventKind::Leave { .. } => "leave",
                    PointerEventKind::Press { .. } => "press",
                    PointerEventKind::Release { .. } => "release",
                    _ => continue,
                };
                println!(
                    "pointer-{event} generation={} x={x:.2} y={y:.2}",
                    surface.generation
                );
            }
        }
    }
}

impl Dispatch<wl_keyboard::WlKeyboard, ()> for Probe {
    fn event(
        state: &mut Self,
        _: &wl_keyboard::WlKeyboard,
        event: wl_keyboard::Event,
        _: &(),
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        match event {
            wl_keyboard::Event::Enter { surface, .. } if state.current(&surface).is_some() => {
                println!("keyboard-enter")
            }
            wl_keyboard::Event::Leave { surface, .. } if state.current(&surface).is_some() => {
                println!("keyboard-leave")
            }
            wl_keyboard::Event::Key {
                key,
                state: key_state,
                ..
            } => {
                #[cfg(feature = "native")]
                if key == 1 // KEY_ESC (Wayland reports Linux evdev codes).
                    && key_state == wayland_client::WEnum::Value(wl_keyboard::KeyState::Pressed)
                    && state.output_transfer.is_none()
                    && matches!(&state.edit, Some(Edit::Screen(edit)) if edit.pending.is_none())
                {
                    if let Err(error) = state.finish_screen_edit(false) {
                        eprintln!("terminal screen edit cancel: {error}");
                    }
                }
                println!("keyboard-key code={key} state={key_state:?}")
            }
            _ => {}
        }
    }
}

impl Dispatch<WpFractionalScaleV1, u32> for Probe {
    fn event(
        state: &mut Self,
        _: &WpFractionalScaleV1,
        event: wp_fractional_scale_v1::Event,
        generation: &u32,
        _: &Connection,
        _: &QueueHandle<Self>,
    ) {
        let wp_fractional_scale_v1::Event::PreferredScale { scale } = event else {
            return;
        };
        if let Some(surface) = state.surface.as_mut().filter(|surface| {
            surface.generation == *generation
                && scale > 0
                && surface.fractional_scale != Some(scale)
        }) {
            println!("preferred-scale generation={generation} numerator={scale}");
            surface.fractional_scale = Some(scale);
            surface.redraw = true;
        }
    }
}

impl ShmHandler for Probe {
    fn shm_state(&mut self) -> &mut Shm {
        &mut self.shm
    }
}
impl ProvidesRegistryState for Probe {
    fn registry(&mut self) -> &mut RegistryState {
        &mut self.registry
    }
    registry_handlers![OutputState, SeatState];
}

delegate_compositor!(Probe);
delegate_layer!(Probe);
delegate_output!(Probe);
delegate_pointer!(Probe);
delegate_registry!(Probe);
delegate_seat!(Probe);
delegate_shm!(Probe);
delegate_xdg_shell!(Probe);
delegate_xdg_window!(Probe);
delegate_noop!(Probe: ignore WpFractionalScaleManagerV1);
delegate_noop!(Probe: ignore WpViewporter);
delegate_noop!(Probe: ignore WpViewport);

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(feature = "native")]
    #[test]
    fn explicit_output_centers_in_destination_and_keeps_zoom() {
        // 目标逻辑尺寸不同时，按已缩放模型居中，同时清除旧输出坐标和裁剪。
        let initial = Transform::new(100.0, 80.0, 1.5, Crop::new(50, 40, 200, 150)).unwrap();
        assert_eq!(
            centered_transform(initial.placement(), Size(1200, 800)),
            Transform::new(360.0, 220.0, 1.5, None).unwrap(),
        );
    }

    #[cfg(feature = "native")]
    #[test]
    #[ignore = "requires a local Cubism 5-r.5 SDK and an EGL driver"]
    fn scene_observes_native_expression_toggle_expiry_after_render() {
        // 用 Haru 同时开启限时与常驻表情，确认渲染推进后仅移除已到期项。
        let sdk = std::env::var_os("CUBISM_SDK_DIR").expect("set CUBISM_SDK_DIR");
        let model = Path::new(&sdk).join("Samples/Resources/Haru/Haru.model3.json");
        let runtime = cubism_bridge::Runtime::new(MODEL_SIZE.0, MODEL_SIZE.1).unwrap();
        let mut frame = native_frame(&runtime, None, false, Some(0.1));
        (frame.render)(
            SceneAction::Load(model, &mut Vec::new(), &mut Vec::new()),
            &mut frame.pixels,
        )
        .unwrap();
        let timed = PathBuf::from("expressions/F01.exp3.json");
        let persistent = PathBuf::from("expressions/F02.exp3.json");
        for (file, deactivate_after) in [(&timed, Some(0.05)), (&persistent, None)] {
            let mut active = false;
            (frame.render)(
                SceneAction::ToggleExpression {
                    file,
                    fade_seconds: 0.2,
                    deactivate_after,
                    active: &mut active,
                },
                &mut frame.pixels,
            )
            .unwrap();
            assert!(active);
            frame.active_expression_toggles.push(file.clone());
        }
        (frame.render)(SceneAction::Draw(0.025, None), &mut frame.pixels).unwrap();
        (frame.render)(
            SceneAction::RetainActiveExpressions(&mut frame.active_expression_toggles),
            &mut frame.pixels,
        )
        .unwrap();
        assert_eq!(frame.active_expression_toggles, [timed, persistent.clone()]);
        (frame.render)(SceneAction::Draw(0.05, None), &mut frame.pixels).unwrap();
        (frame.render)(
            SceneAction::RetainActiveExpressions(&mut frame.active_expression_toggles),
            &mut frame.pixels,
        )
        .unwrap();
        assert_eq!(frame.active_expression_toggles, [persistent]);
    }

    #[cfg(feature = "native")]
    #[test]
    fn global_gaze_sampling_resets_on_disable_and_falls_back_on_invalid_samples() {
        // 用手动时间验证 16 ms 采样上限，并在关闭后清空缓存和定时器。
        let mut gaze = Gaze::default();
        let mut samples = [Some((3.0, 4.0)), Some((f64::NAN, 4.0)), Some((8.0, 9.0))].into_iter();
        let mut provider: GlobalGaze = Box::new(move |connector, size| {
            assert_eq!(connector, "DP-1");
            assert_eq!(size, (800, 600));
            samples.next().unwrap()
        });
        let output = ("DP-1".to_owned(), Size(800, 600));
        let now = Instant::now();
        assert!(gaze.sample(Some(&mut provider), Some(&output), now));
        assert_eq!(gaze.target, Some((3.0, 4.0)));
        assert!(!gaze.sample(
            Some(&mut provider),
            Some(&output),
            now + Duration::from_millis(15)
        ));
        assert!(gaze.sample(
            Some(&mut provider),
            Some(&output),
            now + Duration::from_millis(16)
        ));
        assert_eq!(gaze.target, None);
        assert!(!gaze.sample(Some(&mut provider), None, now + Duration::from_millis(17)));
        assert!(gaze.next.is_none());
        assert!(gaze.sample(
            Some(&mut provider),
            Some(&output),
            now + Duration::from_millis(18)
        ));
        assert_eq!(gaze.target, Some((8.0, 9.0)));
    }

    #[test]
    fn computes_buffer_size_from_logical_size() {
        // 使用奇数尺寸验证 1.5 倍时半像素向上取整，同时覆盖整数缩放和缓冲尺寸限制。
        let logical = Size(321, 241);
        assert_eq!(
            Scale::Fractional(180).pixels(logical).unwrap(),
            Size(482, 362)
        );
        assert_eq!(Scale::Integer(2).pixels(logical).unwrap(), Size(642, 482));
        assert!(Scale::Integer(2).pixels(Size(4096, 4096)).is_err());
    }

    #[test]
    fn paints_transparent_and_premultiplied_model_pixels() {
        // 在 1.5 倍缓冲区中检查透明角及两个逻辑命中矩形内的像素值。
        let logical = OVERLAY_SIZE;
        let pixels = Scale::Fractional(180).pixels(logical).unwrap();
        let mut canvas = vec![0xff; (pixels.0 * pixels.1 * 4) as usize];
        paint(&mut canvas, logical, pixels, false);
        let pixel = |x, y| {
            let index = ((y * pixels.0 + x) * 4) as usize;
            u32::from_ne_bytes(canvas[index..index + 4].try_into().unwrap())
        };
        assert_eq!(pixel(10, 10), 0);
        assert_eq!(pixel(240, 180), 0xc024_9c60);
        assert_eq!(pixel(90, 210), 0x8058_3810);
    }
}
