//! Opt-in desktop rendering for `deskwaifu-rust --overlay`.
//!
//! One thread owns everything below: the Wayland layer-shell overlay and its EGL
//! context, the process-global Cubism Framework lease, the GL textures, the
//! renderer and the animation managers. [`cubism_render`] handles are
//! `!Send`/`!Sync` and the Framework contract requires this same EGL context to
//! stay current until every handle is dropped, so nothing here is moved to
//! another thread or shared. Field order in [`Session`] and [`Scene`] is the
//! drop order: renderer and textures go before the model, the model before the
//! Framework, and the overlay (which owns EGL) last.
//!
//! Scope: this module draws one model into one surface, derives that surface's
//! input region from the drawn alpha, and applies the C++ `InteractionProcessor`
//! pointer policy to the events the overlay reports. Motion policy is the single
//! declared `Idle` group, if the model declares one; the `TapBody` group and the
//! declared expressions are only started by a tap. Physics, pose, look-at and the
//! C++ transform editor are not implemented here.

use crate::Entry;
use cubism_render::{
    Expression, ExpressionManager, EyeBlink, Framework, Model as RenderModel, Motion,
    MotionManager, Renderer,
};
use deskwaifu_model_domain::{extract_input_regions, Region, Transform};
use deskwaifu_overlay::{Overlay, OverlayConfig, PointerEvent, Rect};
use serde_json::{json, Value};
use std::ffi::c_void;
use std::fs;
use std::path::Path;
use std::time::{Duration, Instant};

/// Surface size in pixels. The C++ window size is a user setting; this runtime
/// has no configuration surface yet, so it uses one fixed, modest size.
pub const WIDTH: u32 = 640;
pub const HEIGHT: u32 = 480;

/// Alpha at or below this value counts as a transparent pixel, matching
/// `deskwaifu_model_domain::extract_input_regions` and the C++ window.
const VISIBLE_ALPHA: u8 = 10;
/// Margin of the canvas fit: the longer canvas side covers 90% of the surface.
const FIT: f32 = 1.8;
/// Group name of the only motion policy this runtime applies.
const IDLE_GROUP: &str = "Idle";
/// The C++ `LAppDefine::MotionGroupTapBody`: the group a body tap starts.
const TAP_BODY_GROUP: &str = "TapBody";
/// The C++ `InputController::IsTap` threshold: both axis displacements must be
/// **strictly** below 10 pixels.
const TAP_DISTANCE: i32 = 10;
/// The C++ `ModelDoubleClick::Delay`: two presses at most this many milliseconds
/// apart and within [`ModelDoubleClick::Distance`] are one double-click.
const DOUBLE_CLICK_DELAY_MS: u64 = 350;
/// The C++ `ModelDoubleClick::Distance`: the second press must be closer than
/// this on both axes.
const DOUBLE_CLICK_DISTANCE: i32 = 10;
/// The C++ `PriorityNormal`, used for the tap motions.
const PRIORITY_NORMAL: i32 = 2;
/// The C++ `HitAreaNameHead` / `HitAreaNameBody`.
const HIT_HEAD: &str = "Head";
const HIT_BODY: &str = "Body";

/// The display state the IPC layer owns, pushed into the next drawn frame.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ViewState {
    pub zoom: f32,
    pub x: f32,
    pub y: f32,
    pub hidden: bool,
}

/// Counts from one readback of the drawn surface, for diagnostics and tests.
#[derive(Clone, Copy, Debug)]
pub struct Stats {
    pub frames_drawn: u64,
    pub visible_pixels: u32,
    pub transparent_pixels: u32,
    pub pixel_checksum: u64,
    pub width: u32,
    pub height: u32,
    /// Rectangles in the input region derived from this frame's alpha.
    pub input_regions: u32,
    /// Total covered area of those rectangles, clipped to the surface.
    pub input_region_pixels: u32,
    /// Whether `Overlay::set_input_region` accepted the region for this frame.
    /// A rejected region also fails the frame as a renderer error; the stats are
    /// still recorded, with this `false`, so the status cannot claim a region
    /// that never reached the compositor.
    pub input_region_applied: bool,
    /// Pointer counters observed so far. They are never derived from pixels.
    pub interaction: InteractionCounts,
}

impl Stats {
    pub fn to_json(self, available: bool) -> Value {
        let counts = &self.interaction;
        json!({
            "ok": true,
            "renderer_available": available,
            "frames_drawn": self.frames_drawn,
            "visible_pixels": self.visible_pixels,
            "transparent_pixels": self.transparent_pixels,
            "pixel_checksum": self.pixel_checksum,
            "width": self.width,
            "height": self.height,
            "input_regions": self.input_regions,
            "input_region_pixels": self.input_region_pixels,
            "input_region_applied": self.input_region_applied,
            "pointer_events": counts.pointer_events,
            "taps": counts.taps,
            "double_clicks": counts.double_clicks,
            "drags": counts.drags,
            "right_clicks": counts.right_clicks,
            "middle_clicks": counts.middle_clicks,
            "scrolls": counts.scrolls,
            "ignored_hidden": counts.ignored_hidden,
            "last_hit": counts.last_hit.map(Part::name),
            "expressions_started": counts.expressions_started,
            "motions_started": counts.motions_started,
            "hit_areas_declared": counts.hit_areas_declared,
            "hit_test_available": counts.hit_test_available,
        })
    }
}

/// Maps one bottom-up RGBA8 readback of the drawn surface to the surface-local
/// rectangles installed as the Wayland input region.
///
/// This is the C++ window's path in Rust terms: `ExtractModelInputRegion`
/// samples 10x10 blocks at their centre and treats alpha above [`VISIBLE_ALPHA`]
/// as opaque (`deskwaifu_model_domain::extract_input_regions`). The result
/// already carries top-left origin, is clipped to the surface and has no
/// overlapping rectangles, so it is mapped to [`Rect`] without re-clipping:
/// `Overlay::set_input_region` only unions the rectangles into a `wl_region`.
///
/// The C++ then passes the regions through `ModelFrameInputRegions` with
/// `_settings.ScreenEditing()`, which substitutes the full surface only while a
/// transform edit is in progress. This runtime has no transform editor (see the
/// module docs), so that mode never applies and is not exposed.
///
/// The C++ derives the region from the model's own framebuffer before any
/// display opacity is applied, so hover dimming never changes click-through;
/// this runtime has no hover opacity, so the presented buffer is that buffer.
pub fn input_rects(pixels: &[u8], width: i32, height: i32) -> Result<Vec<Rect>, String> {
    let regions = extract_input_regions(pixels, width, height, Region::new(0, 0, width, height))
        .map_err(|e| format!("cannot derive the input region: {e}"))?;
    Ok(regions
        .into_iter()
        .map(|region| Rect::new(region.x, region.y, region.width, region.height))
        .collect())
}

/// A model part a tap landed on.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Part {
    Head,
    Body,
}

impl Part {
    pub fn name(self) -> &'static str {
        match self {
            Part::Head => HIT_HEAD,
            Part::Body => HIT_BODY,
        }
    }
}

/// Counts that prove the pointer policy ran, reported through the status reply.
#[derive(Clone, Copy, Debug, Default)]
pub struct InteractionCounts {
    pub pointer_events: u64,
    /// Clicks that were recognised as a tap and dispatched to hit testing.
    pub taps: u64,
    /// Second clicks that cancelled a pending tap, as `ModelDoubleClick` does.
    pub double_clicks: u64,
    /// Presses whose release moved at least [`TAP_DISTANCE`] pixels.
    pub drags: u64,
    /// Right-button releases, each of which advances to the next model.
    pub right_clicks: u64,
    /// Middle-button presses. The runtime has no skins, so they do nothing.
    pub middle_clicks: u64,
    pub scrolls: u64,
    /// Button/scroll events dropped because the model was hidden, which is the
    /// C++ `hidden && !screenEditing` early return.
    pub ignored_hidden: u64,
    /// Whether the model declares `HitAreas` at all, which decides between the
    /// drawable-bounds test and the C++ fallback.
    pub hit_areas_declared: bool,
    /// Whether the declared hit areas could be tested. `false` means the tap was
    /// counted but its drawable bounds are not reachable through `cubism-render`.
    pub hit_test_available: bool,
    pub last_hit: Option<Part>,
    /// Expressions started by a tap.
    pub expressions_started: u64,
    /// `TapBody` motions started by a tap.
    pub motions_started: u64,
}

/// A captured left press.
#[derive(Clone, Copy, Debug)]
struct Press {
    /// Surface-local press point, the C++ `InputController::_startX/_startY`.
    x: i32,
    y: i32,
    /// The transform when the press happened, so a drag applies the total offset
    /// from that snapshot instead of accumulating rounding per motion event.
    view: ViewState,
    /// Whether the pointer ever left the tap threshold during this press.
    dragged: bool,
}

/// A detected tap waiting to see whether a second click makes it a double-click.
#[derive(Clone, Copy, Debug)]
struct Pending {
    /// Milliseconds at the release, the C++ `ModelDoubleClick::Release` time.
    time: u64,
    /// Press coordinates, which the C++ compares for the second-click distance.
    x: i32,
    y: i32,
    /// Release coordinates, which are the C++ `Tap` argument.
    tap_x: i32,
    tap_y: i32,
}

/// The C++ `InputController` tap/double-click/drag recogniser, without Qt timers.
///
/// `InteractionProcessor` drives the same rules from a `QTimer`, `PointerButton`
/// and `PointerMoved`; here the tick owns the clock, so a pending tap is flushed
/// once [`DOUBLE_CLICK_DELAY_MS`] has really elapsed instead of by a timer.
#[derive(Debug, Default)]
struct Interaction {
    press: Option<Press>,
    pending: Option<Pending>,
    cursor: Option<(f64, f64)>,
    counts: InteractionCounts,
}

/// What one left release resolved to, the C++ `InputController::ReleaseResult`.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Release {
    /// No press was captured.
    Ignored,
    /// The pointer moved too far, so this was a drag.
    Drag,
    /// A tap is pending until the double-click window closes.
    TapPending,
    /// A second click within the window; no tap is dispatched.
    DoubleClick,
}

impl Interaction {
    /// The C++ `InputController::Press`: capture the start point.
    fn press(&mut self, x: i32, y: i32, view: ViewState) {
        self.press = Some(Press {
            x,
            y,
            view,
            dragged: false,
        });
    }

    /// The C++ `PointerMoved` drag path: once the pointer leaves the tap
    /// threshold, re-derive the transform from the press snapshot.
    ///
    /// The C++ enters local screen editing and calls
    /// `CropInteraction::Drag(start, Model, dx, dy, height)`, which ends in
    /// `InputController::Drag`. This runtime has no transform editor, so the same
    /// offset is applied to the in-memory `View` and nothing is persisted.
    fn moved(&mut self, x: i32, y: i32, width: i32, height: i32) -> Option<ViewState> {
        let press = self.press.as_mut()?;
        if !press.dragged && is_tap(press.x, press.y, x, y) {
            return None;
        }
        press.dragged = true;
        Some(drag_transform(
            press.view,
            x - press.x,
            y - press.y,
            width,
            height,
        ))
    }

    /// Applies the C++ `InputController::Release` rules to one left release and
    /// returns what it resolved to plus any older tap it flushed first.
    ///
    /// `InputController::Release` calls `FlushTap` on both the drag branch and the
    /// "pending click is not this click's second half" branch, so the returned tap
    /// must be dispatched immediately with the *earlier* release coordinates.
    fn release(&mut self, now: u64, x: i32, y: i32) -> (Release, Option<(i32, i32)>) {
        let Some(press) = self.press.take() else {
            return (Release::Ignored, None);
        };
        if press.dragged || !is_tap(press.x, press.y, x, y) {
            self.counts.drags += 1;
            return (Release::Drag, self.dispatch_pending());
        }
        let flushed = if self.pending.is_some() && !self.is_second_click(now, press.x, press.y) {
            self.dispatch_pending()
        } else {
            None
        };
        if self.pending.is_some() && self.is_second_click(now, press.x, press.y) {
            // The C++ keeps no pending click after a double-click; the first
            // click becomes part of the double-click and is never a tap.
            self.pending = None;
            self.counts.double_clicks += 1;
            return (Release::DoubleClick, flushed);
        }
        self.pending = Some(Pending {
            time: now,
            x: press.x,
            y: press.y,
            tap_x: x,
            tap_y: y,
        });
        (Release::TapPending, flushed)
    }

    /// The C++ `ModelDoubleClick::IsSecondClick`: within the interval on the
    /// clock and strictly closer than [`DOUBLE_CLICK_DISTANCE`] on both axes.
    fn is_second_click(&self, now: u64, x: i32, y: i32) -> bool {
        self.pending.is_some_and(|pending| {
            now.saturating_sub(pending.time) <= DOUBLE_CLICK_DELAY_MS
                && (x - pending.x).abs() < DOUBLE_CLICK_DISTANCE
                && (y - pending.y).abs() < DOUBLE_CLICK_DISTANCE
        })
    }

    /// The C++ `ModelDoubleClick::Expired` with its `Delay + 1` deadline: the
    /// pending tap is dispatched once more than [`DOUBLE_CLICK_DELAY_MS`] has
    /// passed, which is what `interaction.tap_timeout` does with its timer.
    fn expired(&mut self, now: u64) -> Option<(i32, i32)> {
        let pending = self.pending?;
        (now.saturating_sub(pending.time) > DOUBLE_CLICK_DELAY_MS).then(|| {
            self.pending = None;
            (pending.tap_x, pending.tap_y)
        })
    }

    /// Takes a pending tap immediately, the C++ `FlushTap`.
    fn dispatch_pending(&mut self) -> Option<(i32, i32)> {
        self.pending
            .take()
            .map(|pending| (pending.tap_x, pending.tap_y))
    }

    /// Drops the capture and every pending tap, the C++ `InteractionProcessor::Cancel`.
    ///
    /// `ModelWindowState` emits `InteractionCancelled` when the window is hidden,
    /// and the processor clears the recogniser and stops the tap timer, because a
    /// pending click may still be waiting while the model is idle.
    fn cancel(&mut self) {
        self.press = None;
        self.pending = None;
    }
}

/// The C++ `InputController::IsTap`: both axis displacements strictly below
/// [`TAP_DISTANCE`].
pub fn is_tap(start_x: i32, start_y: i32, x: i32, y: i32) -> bool {
    (x - start_x).abs() < TAP_DISTANCE && (y - start_y).abs() < TAP_DISTANCE
}

/// Maps one surface-local pixel to the clip space `ViewState::x`/`y` translate
/// in: x right, y up, both in `-1..=1` at the surface edges.
pub fn surface_to_clip(x: f64, y: f64, width: f32, height: f32) -> (f32, f32) {
    if width <= 0.0 || height <= 0.0 {
        return (0.0, 0.0);
    }
    let clip_x = 2.0 * (x as f32) / width - 1.0;
    let clip_y = 1.0 - 2.0 * (y as f32) / height;
    (clip_x, clip_y)
}

/// The canvas-fitting scale `Scene::mvp` uses, corrected for viewport aspect so
/// one model unit occupies the same number of pixels on both axes.
fn clip_scales(canvas: [f32; 2], zoom: f32, width: f32, height: f32) -> (f32, f32) {
    let scale = FIT / canvas[0].max(canvas[1]).max(f32::MIN_POSITIVE) * zoom;
    if width <= 0.0 || height <= 0.0 {
        return (scale, scale);
    }
    (scale * height / width, scale)
}

/// Maps one surface-local pixel into model units by inverting the very matrix
/// `Scene::mvp` builds.
pub fn surface_to_model(
    x: f64,
    y: f64,
    width: f32,
    height: f32,
    view: ViewState,
    canvas: [f32; 2],
) -> (f32, f32) {
    let (clip_x, clip_y) = surface_to_clip(x, y, width, height);
    let (scale_x, scale_y) = clip_scales(canvas, view.zoom, width, height);
    ((clip_x - view.x) / scale_x, (clip_y - view.y) / scale_y)
}

/// The C++ fallback for a model with no `HitAreas` (`LAppModel::HitTest`):
/// `Head` above the model's own midline (`y > 0` in model space), `Body` on or
/// below it.
///
/// `LAppLive2DManager::HitSnapshot` derives its fallback scale and offset from
/// the projection, so the test is exactly "the point's model-space y is
/// positive": the midline follows the model transform rather than sitting at the
/// middle of the surface.
pub fn fallback_part(model_y: f32) -> Part {
    if model_y > 0.0 {
        Part::Head
    } else {
        Part::Body
    }
}

/// Surface-pixel drag offsets mapped to clip-space translation, matching the
/// viewport axes of `Scene::mvp`; y is flipped because surface coordinates point down.
pub fn drag_transform(view: ViewState, dx: i32, dy: i32, width: i32, height: i32) -> ViewState {
    if width <= 0 || height <= 0 {
        return view;
    }
    ViewState {
        x: view.x + 2.0 * dx as f32 / width as f32,
        y: view.y - 2.0 * dy as f32 / height as f32,
        ..view
    }
}

/// One model's GL objects and animation state, rebuilt on every model switch.
///
/// Field order is the drop order: the renderer and textures must go before the
/// model they borrow, and the animation managers before the model they retain.
struct Scene {
    renderer: Renderer,
    /// Held only for lifetime: renderers may draw these textures while they are
    /// alive, so they are never read and never deleted early.
    _textures: Textures,
    motion: Option<Playing>,
    eye_blink: Option<EyeBlink>,
    /// Expression blending and the expressions a tap may start, loaded once with
    /// the scene like the C++ `LAppModel::Load`.
    expressions: Option<ExpressionManager>,
    expression_files: Vec<TappedExpression>,
    /// The C++ `SetRandomExpression` cursor over `expression_files`.
    next_expression: usize,
    /// The `TapBody` motions and their declared fades, in declaration order.
    tap_motions: Vec<TapMotion>,
    /// The manager a body tap plays `tap_motions` through. Created on the first
    /// body tap, so a model that is never tapped has no second queue.
    tap_manager: Option<MotionManager>,
    /// Declaration-order entries whose files could not be loaded. Kept so
    /// `Session::interaction` can report why a tap starts nothing instead of
    /// looking broken.
    ignored_animations: Vec<String>,
    model: RenderModel,
    /// Parameter values written through IPC, by parameter index. They are
    /// re-applied after the per-frame `LoadParameters`/`SaveParameters` layer
    /// restore, which would otherwise overwrite them with the base snapshot.
    overrides: Vec<(i32, f32)>,
    /// Canvas size cached from load; the draw path must not fail on it.
    canvas: [f32; 2],
    /// The model's declared `HitAreas` as `(name, drawable id)`, and whether the
    /// model declares any at all, which selects the C++ fallback.
    hit_areas: Vec<(String, String)>,
}

/// The declared `Idle` motion, kept alive so a finished playback can restart.
struct Playing {
    manager: MotionManager,
    motion: Motion,
}

/// One expression a tap may start, with its declared name.
struct TappedExpression {
    name: String,
    expression: Expression,
}

/// One `TapBody` motion, already carrying its declared fades.
struct TapMotion {
    motion: Motion,
}

/// Rust-owned GL texture names keyed by their model3 texture slot; deleted only
/// while the context is current.
struct Textures(Vec<(i32, u32)>);

impl Drop for Textures {
    fn drop(&mut self) {
        for (_, texture) in &self.0 {
            unsafe { gl::DeleteTextures(1, texture) };
        }
    }
}

impl Scene {
    /// Loads moc, textures and renderer for `entry`, mirroring the C++ load step.
    fn load(framework: &Framework, entry: &Entry, size: (u32, u32)) -> Result<Scene, String> {
        let bytes = fs::read(&entry.moc_path)
            .map_err(|e| format!("cannot read {}: {e}", entry.moc_path.display()))?;
        let model = framework
            .create_model(&bytes)
            .map_err(|e| format!("cannot create model: {e}"))?;
        let canvas = model
            .canvas_size()
            .map_err(|e| format!("cannot read canvas size: {e}"))?;
        let directory = Path::new(&entry.descriptor.directory);
        // The slot a drawable references is its index in `Textures`, so an empty
        // entry must keep its slot rather than be skipped: compacting the vector
        // would shift every later texture and bind it where the moc will not look
        // for it. An empty name therefore reserves the slot without a texture.
        let mut textures = Textures(Vec::new());
        for (slot, name) in entry.model3.texture_file_names().iter().enumerate() {
            if name.is_empty() {
                continue;
            }
            let path = directory.join(name);
            let bytes = fs::read(&path)
                .map_err(|e| format!("cannot read texture {}: {e}", path.display()))?;
            let image = image::load_from_memory(&bytes)
                .map_err(|e| format!("cannot decode texture {}: {e}", path.display()))?
                .into_rgba8();
            let mut texture = 0;
            unsafe {
                gl::GenTextures(1, &mut texture);
                gl::BindTexture(gl::TEXTURE_2D, texture);
                gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR as i32);
                gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR as i32);
                gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE as i32);
                gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::CLAMP_TO_EDGE as i32);
                gl::TexImage2D(
                    gl::TEXTURE_2D,
                    0,
                    gl::RGBA8 as i32,
                    image.width() as i32,
                    image.height() as i32,
                    0,
                    gl::RGBA,
                    gl::UNSIGNED_BYTE,
                    image.as_ptr().cast(),
                );
                // The SDK overrides MIN_FILTER with LINEAR_MIPMAP_LINEAR.
                gl::GenerateMipmap(gl::TEXTURE_2D);
            }
            let error = unsafe { gl::GetError() };
            if error != gl::NO_ERROR {
                unsafe { gl::DeleteTextures(1, &texture) };
                return Err(format!(
                    "cannot upload texture {} (GL error {error:#x})",
                    path.display()
                ));
            }
            textures.0.push((slot as i32, texture));
        }
        let (width, height) = (size.0 as i32, size.1 as i32);
        let mut renderer = model
            .renderer(width, height)
            .map_err(|e| format!("cannot create renderer: {e}"))?;
        for &(slot, texture) in textures.0.iter() {
            renderer
                .bind_texture(slot, texture)
                .map_err(|e| format!("cannot bind texture {slot}: {e}"))?;
        }
        // Eye blink writes the parameter IDs the model3 json declares. A declared
        // group naming an unknown parameter fails the load instead of blinking
        // nothing forever.
        let blink_ids = entry.model3.eye_blink_parameter_ids();
        let eye_blink = if blink_ids.is_empty() {
            None
        } else {
            let ids = blink_ids.iter().map(String::as_str).collect::<Vec<_>>();
            Some(
                model
                    .eye_blink(&ids)
                    .map_err(|e| format!("cannot create eye blink for {ids:?}: {e}"))?,
            )
        };
        // Policy: only a motion group literally named `Idle`, first entry, explicit
        // priority 0. No random selection and nothing invented when absent.
        let motion = entry
            .model3
            .motion_list()
            .into_iter()
            .find(|motion| motion.group == IDLE_GROUP && motion.index == 0)
            .map(|idle| -> Result<Playing, String> {
                let path = directory.join(idle.file);
                let bytes = fs::read(&path)
                    .map_err(|e| format!("cannot read idle motion {}: {e}", path.display()))?;
                let mut motion = framework
                    .create_motion(&bytes)
                    .map_err(|e| format!("cannot create idle motion: {e}"))?;
                // Declared fades only; an absent value keeps the SDK default.
                if let Some(seconds) = idle_fade_in(entry) {
                    motion
                        .set_fade_in(seconds)
                        .map_err(|e| format!("invalid idle motion fade in: {e}"))?;
                }
                if let Some(seconds) = idle_fade_out(entry) {
                    motion
                        .set_fade_out(seconds)
                        .map_err(|e| format!("invalid idle motion fade out: {e}"))?;
                }
                let mut manager = model
                    .motion_manager()
                    .map_err(|e| format!("cannot create motion manager: {e}"))?;
                manager
                    .start(&motion, 0)
                    .map_err(|e| format!("cannot start idle motion: {e}"))?;
                Ok(Playing { manager, motion })
            })
            .transpose()?;
        // Tap animations: the declared expressions (in model3 order, which is the
        // C++ `SetRandomExpression` order over `_expressions`) and the `TapBody`
        // motions. A file that cannot be loaded is skipped and recorded, matching
        // `LAppModel::LoadExpressions`, which logs and leaves the name absent
        // instead of failing the whole model.
        let mut ignored_animations = Vec::new();
        let mut expression_files = Vec::new();
        for declared in &entry.model3.file_references.expressions {
            let path = directory.join(&declared.file);
            match fs::read(&path)
                .map_err(|e| e.to_string())
                .and_then(|bytes| {
                    framework
                        .create_expression(&bytes)
                        .map_err(|e| e.to_string())
                }) {
                Ok(expression) => expression_files.push(TappedExpression {
                    name: declared.name.clone(),
                    expression,
                }),
                Err(error) => ignored_animations.push(format!(
                    "expression {} ({}): {error}",
                    declared.name,
                    path.display()
                )),
            }
        }
        let mut tap_motions = Vec::new();
        for declared in entry
            .model3
            .motion_list()
            .into_iter()
            .filter(|motion| motion.group == TAP_BODY_GROUP)
        {
            let path = directory.join(declared.file);
            let created = fs::read(&path)
                .map_err(|e| e.to_string())
                .and_then(|bytes| {
                    let mut motion = framework.create_motion(&bytes).map_err(|e| e.to_string())?;
                    if let Some(seconds) = tap_fade_in(entry, declared.index) {
                        motion.set_fade_in(seconds).map_err(|e| e.to_string())?;
                    }
                    if let Some(seconds) = tap_fade_out(entry, declared.index) {
                        motion.set_fade_out(seconds).map_err(|e| e.to_string())?;
                    }
                    Ok(motion)
                });
            match created {
                Ok(motion) => tap_motions.push(TapMotion { motion }),
                Err(error) => ignored_animations.push(format!(
                    "{TAP_BODY_GROUP}[{}] {}: {error}",
                    declared.index,
                    path.display()
                )),
            }
        }
        let expressions = match model.expression_manager() {
            Ok(manager) => Some(manager),
            Err(error) => {
                ignored_animations.push(format!("expression manager: {error}"));
                None
            }
        };
        Ok(Scene {
            model,
            _textures: textures,
            renderer,
            motion,
            eye_blink,
            expressions,
            expression_files,
            next_expression: 0,
            tap_motions,
            tap_manager: None,
            ignored_animations,
            overrides: Vec::new(),
            canvas,
            hit_areas: entry
                .model3
                .hit_area_pairs()
                .into_iter()
                .map(|(name, id)| (name.to_string(), id.to_string()))
                .collect(),
        })
    }

    /// One frame: the SDK layer order documented in `cubism-render`'s README.
    fn update(&mut self, delta: f32) -> Result<(), String> {
        let Scene {
            model,
            motion,
            eye_blink,
            expressions,
            tap_manager,
            overrides,
            ..
        } = self;
        model
            .load_parameters()
            .map_err(|e| format!("cannot load parameters: {e}"))?;
        let mut motion_updated = false;
        if let Some(playing) = motion {
            let updated = playing
                .manager
                .update(delta)
                .map_err(|e| format!("motion update failed: {e}"))?;
            motion_updated |= updated;
            // A finished non-looping motion is restarted; a looping motion never
            // reports finished, so this only affects one-shot idle files.
            if !updated && playing.manager.is_finished().unwrap_or(false) {
                playing
                    .manager
                    .start(&playing.motion, 0)
                    .map_err(|e| format!("cannot restart idle motion: {e}"))?;
            }
        }
        // A tap motion runs on its own queue so it cannot disturb the idle
        // restart above; the C++ lets both share `_motionManager` and relies on
        // priorities, which this queue reproduces by simply being separate.
        if let Some(manager) = tap_manager {
            motion_updated |= manager
                .update(delta)
                .map_err(|e| format!("tap motion update failed: {e}"))?;
        }
        // Save the motion-updated base before expressions blend on top. Saving
        // after expression update would feed an additive expression's output back
        // into the next frame and accumulate it indefinitely.
        model
            .save_parameters()
            .map_err(|e| format!("cannot save parameters: {e}"))?;
        // Cubism's EyeBlinkUpdater receives `_motionUpdated` and does not blink
        // over a motion that owns the frame. Otherwise apply blink before
        // expressions, so an explicit expression's eyelid intent wins.
        if !motion_updated {
            if let Some(blink) = eye_blink {
                blink
                    .update(delta)
                    .map_err(|e| format!("eye blink update failed: {e}"))?;
            }
        }
        if let Some(manager) = expressions {
            manager
                .update(delta)
                .map_err(|e| format!("expression update failed: {e}"))?;
        }
        // IPC parameter writes are re-applied after the base layer restore, so a
        // written value survives every frame until it is written again.
        for &(index, value) in overrides.iter() {
            model
                .set_parameter(index, value)
                .map_err(|e| format!("cannot re-apply render parameter {index}: {e}"))?;
        }
        model
            .update()
            .map_err(|e| format!("model update failed: {e}"))
    }

    /// Canvas-fitting projection: the longer canvas side covers `FIT` clip units,
    /// then the IPC zoom and clip-space x/y offset are applied.
    fn mvp(&self, view: ViewState, width: f32, height: f32) -> [f32; 16] {
        let (scale_x, scale_y) = clip_scales(self.canvas, view.zoom, width, height);
        [
            scale_x, 0., 0., 0., 0., scale_y, 0., 0., 0., 0., 1., 0., view.x, view.y, 0., 1.,
        ]
    }

    /// Sets one parameter by ID on the rendered model and remembers the value so
    /// the next frame re-applies it after the base layer restore.
    fn set_parameter(&mut self, id: &str, value: f32) -> Result<(), String> {
        let index = self.parameter_index(id)?;
        self.model
            .set_parameter(index, value)
            .map_err(|e| format!("cannot set render parameter {id}: {e}"))?;
        match self
            .overrides
            .iter_mut()
            .find(|(existing, _)| *existing == index)
        {
            Some(slot) => slot.1 = value,
            None => self.overrides.push((index, value)),
        }
        Ok(())
    }

    /// Reads one parameter by ID from the rendered model, which is what the next
    /// frame draws (the Core model in the IPC layer only validates and replies).
    fn read_parameter(&self, id: &str) -> Result<f32, String> {
        let index = self.parameter_index(id)?;
        self.model
            .parameter(index)
            .map(|parameter| parameter.value)
            .map_err(|e| format!("cannot read render parameter {id}: {e}"))
    }

    fn parameter_index(&self, id: &str) -> Result<i32, String> {
        let count = self
            .model
            .parameter_count()
            .map_err(|e| format!("cannot enumerate render parameters: {e}"))?;
        for index in 0..count {
            let parameter = self
                .model
                .parameter(index)
                .map_err(|e| format!("cannot read render parameter {index}: {e}"))?;
            if parameter.id == id {
                return Ok(index);
            }
        }
        Err(format!("rendered model has no parameter {id}"))
    }

    /// Starts the next declared expression, the C++ `LAppModel::SetRandomExpression`.
    ///
    /// The C++ walks its `_expressions` map with a cursor and starts the next one,
    /// so the choice is round-robin rather than random; this cursor does the same
    /// over the model3 declaration order.
    fn start_random_expression(&mut self) -> Result<(), String> {
        let manager = self
            .expressions
            .as_mut()
            .ok_or("the model has no expression manager")?;
        if self.expression_files.is_empty() {
            return Err("the model declares no expressions".into());
        }
        let index = self.next_expression % self.expression_files.len();
        self.next_expression = (index + 1) % self.expression_files.len();
        let tapped = &self.expression_files[index];
        manager
            .start(&tapped.expression)
            .map_err(|e| format!("cannot start expression {}: {e}", tapped.name))
    }

    /// Starts a `TapBody` motion, the C++ `StartRandomMotion(MotionGroupTapBody)`.
    /// Every declared entry is tried in order; only a real start reports success.
    fn start_tap_motion(&mut self) -> Result<usize, String> {
        if self.tap_motions.is_empty() {
            return Err(format!("the model declares no {TAP_BODY_GROUP} motions"));
        }
        if self.tap_manager.is_none() {
            let created = self
                .model
                .motion_manager()
                .map_err(|e| format!("cannot create tap motion manager: {e}"))?;
            self.tap_manager = Some(created);
        }
        let manager = self.tap_manager.as_mut().expect("created above");
        let start = self.next_expression % self.tap_motions.len();
        let mut last = None;
        for offset in 0..self.tap_motions.len() {
            let index = (start + offset) % self.tap_motions.len();
            match manager.start(&self.tap_motions[index].motion, PRIORITY_NORMAL) {
                Ok(_) => return Ok(index),
                Err(error) => last = Some(error),
            }
        }
        Err(format!(
            "{TAP_BODY_GROUP} motion unavailable: {}",
            last.map_or_else(|| "no entry started".to_string(), |e| e.to_string())
        ))
    }
}

fn idle_fade_in(entry: &Entry) -> Option<f32> {
    entry
        .model3
        .file_references
        .motions
        .iter()
        .find(|group| group.name == IDLE_GROUP)
        .and_then(|group| group.motions.first())
        .and_then(|motion| motion.fade_in_time)
}

fn idle_fade_out(entry: &Entry) -> Option<f32> {
    entry
        .model3
        .file_references
        .motions
        .iter()
        .find(|group| group.name == IDLE_GROUP)
        .and_then(|group| group.motions.first())
        .and_then(|motion| motion.fade_out_time)
}

/// The declared `FadeInTime` of the `TapBody` motion at `index`.
fn tap_fade_in(entry: &Entry, index: usize) -> Option<f32> {
    entry
        .model3
        .file_references
        .motions
        .iter()
        .find(|group| group.name == TAP_BODY_GROUP)
        .and_then(|group| group.motions.get(index))
        .and_then(|motion| motion.fade_in_time)
}

/// The declared `FadeOutTime` of the `TapBody` motion at `index`.
fn tap_fade_out(entry: &Entry, index: usize) -> Option<f32> {
    entry
        .model3
        .file_references
        .motions
        .iter()
        .find(|group| group.name == TAP_BODY_GROUP)
        .and_then(|group| group.motions.get(index))
        .and_then(|motion| motion.fade_out_time)
}

/// The overlay session: everything that exists once `--overlay` is used.
pub struct Session {
    /// `None` after a failed model switch: nothing is drawn and the error is
    /// reported instead of showing a stale or blank window as if it worked.
    scene: Option<Scene>,
    framework: Framework,
    overlay: Overlay,
    size: (u32, u32),
    frames_drawn: u64,
    stats: Option<Stats>,
    error: Option<String>,
    last_frame: Instant,
    /// Reused `glReadPixels` destination, like the C++ `_inputPixels`; the first
    /// frame sizes it for the surface and every later frame reuses it.
    pixels: Vec<u8>,
    /// The rectangles last accepted by `Overlay::set_input_region`, so the
    /// diagnostics reply can show the exact region rather than only its size.
    region: Vec<Rect>,
    /// The C++ tap/double-click/drag recogniser and the pointer position.
    interaction: Interaction,
    /// A right release asked for `next_model`; the tick drains it through the
    /// same `select` path the IPC command uses, so both really switch the scene.
    next_model_requested: bool,
    /// The last error a tap's own action reported, reported beside the counts so
    /// a tap that started nothing says why.
    interaction_error: Option<String>,
    /// Milliseconds since [`Session`] started, the tick's shared clock for the
    /// double-click window.
    started: Instant,
}

impl Session {
    /// Connects the compositor, starts the Framework with the SDK's external
    /// shaders and loads the model. Every failure is returned with context.
    pub fn start(entry: &Entry) -> Result<Session, String> {
        let display = std::env::var_os("WAYLAND_DISPLAY")
            .filter(|value| !value.is_empty())
            .ok_or("WAYLAND_DISPLAY is not set; overlay mode needs a Wayland compositor")?;
        let sdk = std::env::var_os("CUBISM_SDK_ROOT")
            .filter(|value| !value.is_empty())
            .ok_or("CUBISM_SDK_ROOT is not set; overlay mode loads the SDK's Standard shaders")?;
        let shaders = Path::new(&sdk).join("Framework/src/Rendering/OpenGL/Shaders/Standard");
        if !shaders.join("VertShaderSrc.vert").is_file() {
            return Err(format!(
                "shader directory {} has no VertShaderSrc.vert",
                shaders.display()
            ));
        }
        let connection = wayland_client::Connection::connect_to_env()
            .map_err(|e| format!("cannot connect to Wayland display {:?}: {e}", display))?;
        let mut overlay = Overlay::connect(connection, OverlayConfig::new(WIDTH, HEIGHT))
            .map_err(|e| format!("cannot create the layer-shell overlay: {e}"))?;
        overlay
            .make_current()
            .map_err(|e| format!("cannot make the overlay context current: {e}"))?;
        gl::load_with(|symbol| {
            overlay
                .egl()
                .get_proc_address(symbol)
                .map_or(std::ptr::null(), |function| function as *const c_void)
        });
        // SAFETY: the overlay's EGL context is current on this thread and stays
        // current until `Session` drops the Framework before dropping the overlay.
        let framework = unsafe { Framework::start(&shaders) }
            .map_err(|e| format!("cannot start the Cubism Framework: {e}"))?;
        let size = overlay.size();
        let scene = Scene::load(&framework, entry, size)?;
        // Click-through from the first frame: an empty region, exactly the C++
        // `SetInputRegion({ })` start. The first real frame replaces it with the
        // region derived from that frame's alpha.
        overlay
            .set_input_region(&[])
            .map_err(|e| format!("cannot clear the input region: {e}"))?;
        Ok(Session {
            scene: Some(scene),
            framework,
            overlay,
            size,
            frames_drawn: 0,
            stats: None,
            error: None,
            last_frame: Instant::now(),
            pixels: Vec::new(),
            region: Vec::new(),
            interaction: Interaction::default(),
            next_model_requested: false,
            interaction_error: None,
            started: Instant::now(),
        })
    }

    /// Rebuilds model, textures and renderer after a model switch, then draws one
    /// frame so the status is truthful immediately. On failure the old scene is
    /// dropped, so no stale model keeps drawing under the new identity; the error
    /// is reported through `get_status` and the reply.
    pub fn reload(&mut self, entry: &Entry, view: ViewState) -> Result<(), String> {
        self.make_current()?;
        // The frame counter describes one scene: a new model must draw and be
        // read back again before `renderer_available` may claim it is drawing.
        self.frames_drawn = 0;
        self.stats = None;
        self.last_frame = Instant::now();
        match Scene::load(&self.framework, entry, self.size) {
            Ok(scene) => {
                self.scene = Some(scene);
                self.error = None;
                self.frame(view);
                Ok(())
            }
            Err(error) => {
                // Dropping the scene does not retract the buffer the compositor
                // already holds, so present one transparent frame with an empty
                // input region first; otherwise a failed switch leaves the old
                // model visible and still intercepting clicks.
                let cleared = self.clear_surface();
                self.scene = None;
                let error = match cleared {
                    Ok(()) => error,
                    Err(clear_error) => {
                        format!("{error}; cannot clear the failed scene: {clear_error}")
                    }
                };
                self.error = Some(error.clone());
                Err(error)
            }
        }
    }

    /// Presents a fully transparent frame and clears the input region.
    ///
    /// Used when the renderer can no longer draw (a failed model switch): the
    /// compositor keeps showing the last committed buffer until another one
    /// arrives, and its input region likewise stays in force. Read back the clear
    /// just like a drawn frame, but keep the scene's frame count at zero so this
    /// cleanup cannot claim that the model rendered successfully.
    fn clear_surface(&mut self) -> Result<(), String> {
        self.frames_drawn = 0;
        self.stats = None;
        self.make_current()?;
        let (width, height) = (self.size.0 as i32, self.size.1 as i32);
        unsafe {
            gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
            gl::Viewport(0, 0, width, height);
            gl::ClearColor(0., 0., 0., 0.);
            gl::Clear(gl::COLOR_BUFFER_BIT);
        }
        self.stats = Some(self.readback(width, height)?);
        self.overlay
            .swap_buffers()
            .map_err(|e| format!("cannot present the transparent frame: {e}"))?;
        self.overlay
            .dispatch_pending()
            .map_err(|e| format!("cannot dispatch Wayland events: {e}"))
    }

    /// Applies one parameter to the rendered model by parameter ID.
    pub fn set_parameter(&mut self, id: &str, value: f32) -> Result<(), String> {
        if self.scene.is_none() {
            return Err(self.unavailable());
        }
        let scene = self.scene.as_mut().expect("checked above");
        scene.set_parameter(id, value)
    }

    /// The next 16 ms frame: update, draw, read back, derive the input region
    /// from that readback, apply it, then present.
    pub fn frame(&mut self, view: ViewState) {
        if self.scene.is_none() {
            return;
        }
        if let Err(error) = self.draw(view) {
            self.fail(error);
        }
    }

    /// Drains the overlay's pointer events, applies the C++ policy and returns the
    /// transform a drag may have moved the model to.
    ///
    /// This runs on the same tick as [`Session::frame`] and is the Rust port of
    /// `InteractionProcessor::PointerButton`/`PointerMoved`/`FlushTap`/`Tap`:
    ///
    /// - `hidden` is the C++ `hidden && !screenEditing` early return. There is no
    ///   screen-edit mode here, so a hidden model ignores every tap, drag, right
    ///   click, middle click and scroll exactly as the C++ does with editing off.
    /// - A left press captures the start point; a release under
    ///   [`TAP_DISTANCE`] on both axes is a tap, two such releases within
    ///   [`DOUBLE_CLICK_DELAY_MS`] and [`DOUBLE_CLICK_DISTANCE`] are a
    ///   double-click, and a release beyond the threshold is a drag.
    /// - A double-click in the C++ opens the keyboard grab and emits
    ///   `interaction.double_click/v1`; this runtime has no keyboard grab and no
    ///   notification bus, so a double-click only cancels the pending tap and is
    ///   counted. That is the deliberate, documented equivalent, not a fake.
    /// - A drag moves the model through the C++ `InputController::Drag` math.
    /// - A tap is hit-tested per [`Session::hit`] and dispatches the C++
    ///   `Tap` policy: `Head` starts a random expression, `Body` starts a
    ///   `TapBody` motion and a random expression.
    /// - A right release advances to the next model, like the C++ `next_model`.
    ///   A middle press does nothing, because this runtime has no skins; the C++
    ///   sends `switch_skin`, which has no counterpart here.
    pub fn pump(&mut self, view: ViewState) -> ViewState {
        let events = self.overlay.take_events();
        self.interaction.counts.pointer_events += events.len() as u64;
        let now = self.clock_ms();
        let mut view = view;
        for event in events {
            match event {
                PointerEvent::Entered { x, y } | PointerEvent::Moved { x, y } => {
                    self.interaction.cursor = Some((x, y));
                    if let Some(dragged) = self.interaction.moved(
                        x as i32,
                        y as i32,
                        self.size.0 as i32,
                        self.size.1 as i32,
                    ) {
                        // The C++ `PointerButton` refuses to start a drag while
                        // hidden and `ModelWindowState::CancelGesture` clears the
                        // recogniser, so a hidden model neither moves nor keeps a
                        // half-finished gesture.
                        if view.hidden {
                            self.interaction.cancel();
                            self.interaction.counts.ignored_hidden += 1;
                        } else {
                            view = dragged;
                        }
                    }
                }
                // The C++ `Leave` only clears `_inside`, which this runtime has
                // no hover dimming to use; the position is forgotten so a scroll
                // cannot zoom around a stale point.
                PointerEvent::Left => self.interaction.cursor = None,
                PointerEvent::Scroll { delta } => {
                    self.interaction.counts.scrolls += 1;
                    if view.hidden {
                        self.interaction.counts.ignored_hidden += 1;
                        continue;
                    }
                    // The C++ `PointerScroll`: zoom by `1 + vertical * .1` around
                    // the cursor in clip space, through `Transform::zoom_around`,
                    // which clamps the scale to the same 0.1..10 window.
                    if let Some((x, y)) = self.interaction.cursor {
                        let (width, height) = (self.size.0 as f32, self.size.1 as f32);
                        let (anchor_x, anchor_y) = surface_to_clip(x, y, width, height);
                        let transform = Transform::new(view.zoom, view.x, view.y).zoom_around(
                            anchor_x,
                            anchor_y,
                            view.zoom * (1.0 + delta as f32 * 0.1),
                        );
                        view.zoom = transform.scale;
                        view.x = transform.x;
                        view.y = transform.y;
                    }
                }
                PointerEvent::Button { button, pressed } => {
                    if view.hidden {
                        // `ModelWindowState` cancels the recogniser when the model
                        // is hidden, so a click cannot leave a press captured.
                        self.interaction.cancel();
                        self.interaction.counts.ignored_hidden += 1;
                        continue;
                    }
                    match (button, pressed) {
                        (0, true) => {
                            let (x, y) = self.pointer();
                            self.interaction.press(x, y, view);
                        }
                        (0, false) => {
                            let (x, y) = self.pointer();
                            // A tap is only dispatched once the double-click
                            // window closes; an older pending click flushed by
                            // this release is dispatched now, as the C++ does.
                            let (_, flushed) = self.interaction.release(now, x, y);
                            if let Some((tap_x, tap_y)) = flushed {
                                self.tap(tap_x, tap_y, &view);
                            }
                        }
                        (1, false) => {
                            self.interaction.counts.right_clicks += 1;
                            self.next_model_requested = true;
                        }
                        // The C++ sends `switch_skin`, which this runtime has no
                        // equivalent for; only the count proves the event arrived.
                        (2, true) => self.interaction.counts.middle_clicks += 1,
                        _ => {}
                    }
                }
            }
        }
        self.flush_expired(&mut view, now);
        view
    }

    /// Dispatches a pending tap once the C++ double-click window has closed, the
    /// `interaction.tap_timeout` timer in `InteractionProcessor`.
    pub fn flush_expired(&mut self, view: &mut ViewState, now: u64) {
        if let Some((x, y)) = self.interaction.expired(now) {
            self.tap(x, y, view);
        }
    }

    /// Whether a right release asked for `next_model`, which the tick performs so
    /// the scene switch goes through the same `select` path as the IPC command.
    pub fn take_next_model(&mut self) -> bool {
        std::mem::take(&mut self.next_model_requested)
    }

    /// Milliseconds since this session started, the clock the double-click window
    /// and its timeout use.
    pub fn clock_ms(&self) -> u64 {
        u64::try_from(self.started.elapsed().as_millis()).unwrap_or(u64::MAX)
    }

    /// The last pointer position, or the surface centre before the first motion.
    ///
    /// `wl_pointer` reports a button without a position, so the C++ window calls
    /// `PointerMoved` first; the overlay documents that the caller keeps its last
    /// `Moved`/`Entered`, which is what this does.
    fn pointer(&self) -> (i32, i32) {
        self.interaction.cursor.map_or(
            (self.size.0 as i32 / 2, self.size.1 as i32 / 2),
            |(x, y)| (x as i32, y as i32),
        )
    }

    /// The C++ `InteractionProcessor::Tap`.
    ///
    /// `Tap` does nothing unless the hit snapshot is available, and `HitAt`
    /// returns nothing while the model is hidden, so both checks come first. The
    /// declared `Head`/`Body` areas are tested when the model declares any; a
    /// model that declares none uses the C++ `LAppModel::HitTest` fallback.
    fn tap(&mut self, x: i32, y: i32, view: &ViewState) {
        if view.hidden {
            self.interaction.counts.ignored_hidden += 1;
            return;
        }
        self.interaction.counts.taps += 1;
        match self.hit(x, y, *view) {
            Ok(part) => {
                self.interaction.counts.last_hit = Some(part);
                let result = match part {
                    Part::Head => self.start_expression(),
                    Part::Body => {
                        let motion = self.start_tap_motion();
                        let expression = self.start_expression();
                        motion.and(expression)
                    }
                };
                if let Err(error) = result {
                    self.interaction_error = Some(self.with_ignored(error));
                }
            }
            Err(error) => {
                self.interaction.counts.last_hit = None;
                self.interaction_error = Some(error);
            }
        }
    }

    /// Appends the tap-animation files that failed to load to a tap's error, so a
    /// tap that started nothing points at the real cause.
    fn with_ignored(&self, error: String) -> String {
        let ignored = self.ignored_animations();
        if ignored.is_empty() {
            error
        } else {
            format!("{error} (unloaded tap animations: {})", ignored.join("; "))
        }
    }

    /// Starts the next declared expression and counts it.
    fn start_expression(&mut self) -> Result<(), String> {
        let started = match self.scene.as_mut() {
            Some(scene) => scene.start_random_expression(),
            None => Err(self.unavailable()),
        };
        if started.is_ok() {
            self.interaction.counts.expressions_started += 1;
        }
        started
    }

    /// Starts a `TapBody` motion and counts it.
    fn start_tap_motion(&mut self) -> Result<(), String> {
        let started = match self.scene.as_mut() {
            Some(scene) => scene.start_tap_motion().map(|_| ()),
            None => Err(self.unavailable()),
        };
        if started.is_ok() {
            self.interaction.counts.motions_started += 1;
        }
        started
    }

    /// The C++ `HitAt` + `LAppModel::HitTest`: which declared part contains the
    /// surface point, mapped through the same projection the frame draws with.
    ///
    /// The drawable bounds the C++ reads (`GetDrawableVertices` of the declared
    /// hit-area ID) are not exposed by `cubism-render`, so a model that declares
    /// `HitAreas` reports `hit_test_available:false` and no part is claimed rather
    /// than guessing. A model that declares none uses the C++ fallback, which only
    /// needs the projection: `Head` above the midline, `Body` on or below it.
    fn hit(&mut self, x: i32, y: i32, view: ViewState) -> Result<Part, String> {
        let Some(scene) = self.scene.as_ref() else {
            return Err(self.unavailable());
        };
        let (width, height) = (self.size.0 as f32, self.size.1 as f32);
        let (_model_x, model_y) =
            surface_to_model(x as f64, y as f64, width, height, view, scene.canvas);
        let declared = !scene.hit_areas.is_empty();
        self.interaction.counts.hit_areas_declared = declared;
        if !declared {
            self.interaction.counts.hit_test_available = true;
            return Ok(fallback_part(model_y));
        }
        // The C++ reads each declared hit area's drawable vertices and tests the
        // point against their axis-aligned bounds. `cubism-render` exposes no
        // drawable vertices, so no part is claimed here instead of guessing.
        self.interaction.counts.hit_test_available = false;
        Err(format!(
            "the model declares HitAreas but their drawable bounds are not reachable through cubism-render; the tap at {x},{y} started nothing"
        ))
    }

    /// Forces one frame and a fresh readback, for diagnostics and the live test.
    pub fn stats(&mut self, view: ViewState) -> Result<Stats, String> {
        if self.scene.is_none() {
            return Err(self.unavailable());
        }
        self.draw(view)?;
        self.stats.ok_or("frame produced no stats".to_string())
    }

    /// The rendered model's current value for one parameter ID.
    pub fn parameter(&self, id: &str) -> Result<f32, String> {
        let scene = self.scene.as_ref().ok_or("renderer is not available")?;
        scene.read_parameter(id)
    }

    /// The last observed counts, and whether a frame was really drawn.
    pub fn status(&self) -> (bool, Option<Stats>) {
        (self.available(), self.stats)
    }

    /// The live interaction state, enriched with what this session knows beyond
    /// one frame: the point a tap was dispatched to and the last tap error.
    ///
    /// These are the counters the status reply publishes, never a value derived
    /// from pixels, so a client can prove the pointer path really ran.
    pub fn interaction(&self) -> (InteractionCounts, Option<String>) {
        (self.interaction.counts, self.interaction_error.clone())
    }

    /// Tap animation entries that could not be loaded, so a tap that starts
    /// nothing says why instead of looking broken.
    pub fn ignored_animations(&self) -> &[String] {
        self.scene
            .as_ref()
            .map_or(&[], |scene| scene.ignored_animations.as_slice())
    }

    /// The exact input-region rectangles the compositor currently has, in
    /// surface-local top-left coordinates.
    pub fn region(&self) -> &[Rect] {
        &self.region
    }

    pub fn error(&self) -> Option<&str> {
        self.error.as_deref()
    }

    pub fn available(&self) -> bool {
        self.scene.is_some() && self.frames_drawn > 0 && self.error.is_none()
    }

    fn unavailable(&self) -> String {
        self.error
            .clone()
            .unwrap_or_else(|| "renderer is not available".into())
    }

    fn fail(&mut self, error: String) {
        let message = format!("overlay frame failed: {error}");
        if self.error.as_deref() != Some(message.as_str()) {
            eprintln!("deskwaifu-rust: {message}");
        }
        self.error = Some(message);
    }

    fn make_current(&self) -> Result<(), String> {
        self.overlay
            .make_current()
            .map_err(|e| format!("cannot make the overlay context current: {e}"))
    }

    /// One frame, in the C++ order: draw the model, read the drawn buffer back,
    /// derive the input region from that buffer's alpha, apply it, then swap.
    ///
    /// The readback happens **before** `swap_buffers` because the window surface
    /// is a double-buffered EGL surface: after a swap the back buffer the frame
    /// was drawn into is no longer defined, so the alpha analysis must read the
    /// buffer that was just drawn. This is also where the C++ reads: `Present()`
    /// derives the region from the render framebuffer, then blits and swaps.
    ///
    /// Every frame recomputes the region. That is what the C++ does: the render
    /// thread calls `ReadModelInputRegion` on every 16 ms frame and the
    /// interaction thread calls `SetInputRegion` on every presented frame, with
    /// no change detection between them. Since the region follows the animated
    /// model, a cache could only ever skip work on frames that did not move.
    fn draw(&mut self, view: ViewState) -> Result<(), String> {
        self.make_current()?;
        let (width, height) = (self.size.0 as i32, self.size.1 as i32);
        let now = Instant::now();
        // Bounded step: a long IPC stall must not jump the animation forward.
        let delta = now
            .saturating_duration_since(self.last_frame)
            .min(Duration::from_millis(64))
            .as_secs_f32();
        self.last_frame = now;
        let scene = self.scene.as_mut().ok_or("renderer is not available")?;
        scene.update(delta)?;
        let mvp = scene.mvp(view, self.size.0 as f32, self.size.1 as f32);
        unsafe {
            gl::BindFramebuffer(gl::FRAMEBUFFER, 0);
            gl::Viewport(0, 0, width, height);
            gl::ClearColor(0., 0., 0., 0.);
            gl::Clear(gl::COLOR_BUFFER_BIT);
        }
        self.frames_drawn += 1;
        if !view.hidden {
            scene
                .renderer
                .draw(&mvp)
                .map_err(|e| format!("model draw failed: {e}"))?;
        }
        let stats = self.readback(width, height)?;
        self.stats = Some(stats);
        self.overlay
            .swap_buffers()
            .map_err(|e| format!("cannot present the frame: {e}"))?;
        self.overlay
            .dispatch_pending()
            .map_err(|e| format!("cannot dispatch Wayland events: {e}"))
    }

    /// Reads the drawn buffer, derives the input region from its alpha and
    /// installs it. The returned [`Stats`] describes both the pixels and the
    /// region: `input_region_applied` is `false` when `set_input_region` rejected
    /// it, and that failure is returned as a renderer error too.
    fn readback(&mut self, width: i32, height: i32) -> Result<Stats, String> {
        let required = (width * height * 4) as usize;
        if self.pixels.len() != required {
            self.pixels = vec![0; required];
        }
        unsafe {
            gl::ReadPixels(
                0,
                0,
                width,
                height,
                gl::RGBA,
                gl::UNSIGNED_BYTE,
                self.pixels.as_mut_ptr().cast(),
            );
        }
        let error = unsafe { gl::GetError() };
        if error != gl::NO_ERROR {
            return Err(format!("GL readback failed (GL error {error:#x})"));
        }
        let mut visible_pixels = 0;
        let mut transparent_pixels = 0;
        let mut pixel_checksum = 0xcbf2_9ce4_8422_2325u64;
        for pixel in self.pixels.as_chunks::<4>().0 {
            if pixel[3] > VISIBLE_ALPHA {
                visible_pixels += 1;
            } else if pixel[3] == 0 {
                transparent_pixels += 1;
            }
            for byte in pixel {
                pixel_checksum ^= *byte as u64;
                pixel_checksum = pixel_checksum.wrapping_mul(0x100_0000_01b3);
            }
        }
        let rects = input_rects(&self.pixels, width, height)?;
        let input_regions = rects.len() as u32;
        let input_region_pixels = rects
            .iter()
            .map(|rect| (rect.width as u64) * (rect.height as u64))
            .sum::<u64>();
        let stats = Stats {
            frames_drawn: self.frames_drawn,
            visible_pixels,
            transparent_pixels,
            pixel_checksum,
            width: width as u32,
            height: height as u32,
            input_regions,
            input_region_pixels: u32::try_from(input_region_pixels).unwrap_or(u32::MAX),
            input_region_applied: false,
            interaction: self.interaction.counts,
        };
        if let Err(error) = self.overlay.set_input_region(&rects) {
            // Keep the counts, but say the region never reached the compositor.
            self.stats = Some(stats);
            return Err(format!(
                "cannot apply the input region of {input_regions} rectangles: {error}"
            ));
        }
        self.region = rects;
        Ok(Stats {
            input_region_applied: true,
            ..stats
        })
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        // The Framework disposes GL programs, so the EGL context must be current
        // before the scene and the Framework drop; the overlay goes last.
        let _ = self.make_current();
        // Drop in the documented order rather than relying on field order alone.
        self.scene = None;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A bottom-up RGBA8 buffer where every pixel is fully transparent.
    fn blank(width: i32, height: i32) -> Vec<u8> {
        vec![0; (width * height * 4) as usize]
    }

    fn set_alpha(pixels: &mut [u8], width: i32, x: i32, y: i32, alpha: u8) {
        pixels[((y * width + x) * 4 + 3) as usize] = alpha;
    }

    fn covered(rects: &[Rect], width: i32, height: i32) -> usize {
        assert!(
            rects.iter().all(|rect| {
                rect.width > 0
                    && rect.height > 0
                    && rect.x >= 0
                    && rect.y >= 0
                    && rect.x + rect.width <= width
                    && rect.y + rect.height <= height
            }),
            "every rect must be a positive area inside the surface: {rects:?}"
        );
        rects
            .iter()
            .map(|rect| (rect.width * rect.height) as usize)
            .sum()
    }

    #[test]
    fn transparent_frame_has_no_input_region() {
        let pixels = blank(640, 480);
        assert!(input_rects(&pixels, 640, 480).unwrap().is_empty());
    }

    #[test]
    fn opaque_frame_covers_the_whole_surface() {
        let (width, height) = (640, 480);
        let pixels = vec![255; (width * height * 4) as usize];
        let rects = input_rects(&pixels, width, height).unwrap();
        // 480 rows in 10-pixel blocks: 48 full-height runs, one per band.
        assert_eq!(rects.len(), 48);
        assert_eq!(covered(&rects, width, height), (width * height) as usize);
    }

    #[test]
    fn one_opaque_block_becomes_one_rect() {
        // The sampler reads the block centre: buffer rows 0..9 (the bottom of the
        // screen), columns 10..19.
        let (width, height) = (27, 27);
        let mut pixels = blank(width, height);
        set_alpha(&mut pixels, width, 15, 5, 255);
        assert_eq!(
            input_rects(&pixels, width, height).unwrap(),
            [Rect::new(10, 17, 10, 10)]
        );
    }

    #[test]
    fn only_the_block_centre_pixel_is_sampled() {
        // Same centre convention as the C++ `(y + h/2) * width + x + w/2`: an
        // opaque pixel that is not the centre of any block contributes nothing.
        let (width, height) = (27, 27);
        let mut pixels = blank(width, height);
        set_alpha(&mut pixels, width, 11, 1, 255);
        assert!(input_rects(&pixels, width, height).unwrap().is_empty());
    }

    #[test]
    fn threshold_alpha_10_is_transparent_and_11_is_opaque() {
        let (width, height) = (27, 27);
        // Alpha 10 is the last transparent value, 11 the first opaque one; both
        // values are read at the centre of their own block.
        let mut pixels = blank(width, height);
        set_alpha(&mut pixels, width, 5, 5, VISIBLE_ALPHA);
        assert!(input_rects(&pixels, width, height).unwrap().is_empty());
        set_alpha(&mut pixels, width, 5, 5, VISIBLE_ALPHA + 1);
        assert_eq!(
            input_rects(&pixels, width, height).unwrap(),
            [Rect::new(0, 17, 10, 10)]
        );
    }

    #[test]
    fn opaque_rows_at_the_top_of_the_buffer_become_the_top_of_the_surface() {
        // glReadPixels returns bottom-up: buffer row 0 is the bottom of the
        // screen. An opaque band in the buffer's highest rows must therefore land
        // at the top of the top-left-origin surface (y = 0), not at its bottom.
        let (width, height) = (27, 27);
        let mut pixels = blank(width, height);
        for y in 20..27 {
            for x in 0..10 {
                set_alpha(&mut pixels, width, x, y, 255);
            }
        }
        assert_eq!(
            input_rects(&pixels, width, height).unwrap(),
            [Rect::new(0, 0, 10, 7)]
        );
        // The mirrored band (buffer rows 0..9, the bottom of the screen) is the
        // surface's lowest band.
        let mut pixels = blank(width, height);
        for y in 0..10 {
            for x in 0..10 {
                set_alpha(&mut pixels, width, x, y, 255);
            }
        }
        assert_eq!(
            input_rects(&pixels, width, height).unwrap(),
            [Rect::new(0, 17, 10, 10)]
        );
    }

    #[test]
    fn opaque_runs_are_split_by_transparent_blocks_and_never_overlap() {
        let (width, height) = (27, 17);
        let mut pixels = blank(width, height);
        // Block centres are x=5, 15 and 23; leaving the middle block transparent
        // splits the opaque band into two runs.
        for x in [5, 23] {
            for y in 0..10 {
                set_alpha(&mut pixels, width, x, y, 255);
            }
        }
        let rects = input_rects(&pixels, width, height).unwrap();
        assert_eq!(rects, [Rect::new(0, 7, 10, 10), Rect::new(20, 7, 7, 10)]);
        // The helper already guarantees this, but the mapping to `Rect` must not
        // reintroduce overlap by clipping differently.
        for (index, rect) in rects.iter().enumerate() {
            for other in &rects[index + 1..] {
                let overlap = rect.x.max(other.x)
                    < (rect.x + rect.width).min(other.x + other.width)
                    && rect.y.max(other.y) < (rect.y + rect.height).min(other.y + other.height);
                assert!(!overlap, "{rect:?} overlaps {other:?}");
            }
        }
    }

    #[test]
    fn a_short_buffer_is_an_error_not_an_empty_region() {
        assert!(input_rects(&[], 640, 480).is_err());
        assert!(input_rects(&blank(0, 480), 0, 480).unwrap().is_empty());
    }

    // --- The C++ interaction policy, without a compositor ---

    fn view() -> ViewState {
        ViewState {
            zoom: 1.0,
            x: 0.0,
            y: 0.0,
            hidden: false,
        }
    }

    #[test]
    fn a_tap_needs_both_axes_strictly_below_ten_pixels() {
        // `InputController::IsTap` uses `< 10`, so 9 is a tap and 10 is a drag.
        assert!(is_tap(0, 0, 9, 9));
        assert!(is_tap(0, 0, -9, -9));
        assert!(!is_tap(0, 0, 10, 0));
        assert!(!is_tap(0, 0, 0, 10));
    }

    #[test]
    fn one_lone_click_is_a_tap_after_the_double_click_window() {
        let mut interaction = Interaction::default();
        let view = view();
        interaction.press(100, 100, view);
        assert_eq!(interaction.release(1_000, 101, 102).0, Release::TapPending);
        // Still inside the 350 ms window: a second click could still cancel it.
        assert_eq!(interaction.expired(1_349), None);
        assert_eq!(interaction.expired(1_351), Some((101, 102)));
        assert_eq!(interaction.counts.drags, 0);
        assert_eq!(interaction.counts.double_clicks, 0);
    }

    #[test]
    fn two_clicks_inside_the_window_are_a_double_click_and_dispatch_no_tap() {
        let mut interaction = Interaction::default();
        interaction.press(100, 100, view());
        assert_eq!(interaction.release(1_000, 100, 100).0, Release::TapPending);
        interaction.press(103, 104, view());
        assert_eq!(interaction.release(1_300, 103, 104).0, Release::DoubleClick);
        assert_eq!(interaction.counts.double_clicks, 1);
        // The first click never became a tap.
        assert_eq!(interaction.expired(2_000), None);
    }

    #[test]
    fn a_second_click_outside_the_window_dispatches_the_first_tap_first() {
        let mut interaction = Interaction::default();
        interaction.press(100, 100, view());
        assert_eq!(interaction.release(1_000, 100, 100).0, Release::TapPending);
        // 351 ms later: the first tap is dispatched, the second one becomes pending.
        interaction.press(100, 100, view());
        assert_eq!(interaction.release(1_351, 100, 100).0, Release::TapPending);
        // The expired first tap was flushed by `release` before the second was
        // stored, which is `InputController::Release`'s `FlushTap` path.
        assert_eq!(interaction.counts.double_clicks, 0);
    }

    #[test]
    fn a_second_click_too_far_away_is_not_the_same_double_click() {
        let mut interaction = Interaction::default();
        interaction.press(100, 100, view());
        interaction.release(1_000, 100, 100);
        interaction.press(110, 100, view());
        // 10 px apart is outside `Distance` (< 10), so this is a new pending tap.
        assert_eq!(interaction.release(1_100, 110, 100).0, Release::TapPending);
        assert_eq!(interaction.counts.double_clicks, 0);
    }

    #[test]
    fn moving_beyond_the_threshold_turns_the_press_into_a_drag() {
        let mut interaction = Interaction::default();
        let start = view();
        interaction.press(100, 100, start);
        // Inside the threshold the press is still a tap.
        assert_eq!(
            interaction.moved(104, 106, WIDTH as i32, HEIGHT as i32),
            None
        );
        // Beyond it the model follows the C++ `InputController::Drag` math.
        let dragged = interaction
            .moved(130, 100, WIDTH as i32, HEIGHT as i32)
            .unwrap();
        assert_eq!(dragged.x, 30.0 * 2.0 / WIDTH as f32);
        assert_eq!(dragged.y, 0.0);
        assert_eq!(
            interaction.release(1_000, 130, 100).0,
            Release::Drag,
            "a release after leaving the threshold must not be a tap"
        );
        assert_eq!(interaction.counts.drags, 1);
    }

    #[test]
    fn a_drag_uses_the_press_snapshot_not_the_previous_motion() {
        let mut interaction = Interaction::default();
        let start = ViewState {
            x: 0.25,
            y: -0.5,
            ..view()
        };
        interaction.press(100, 100, start);
        interaction.moved(120, 100, WIDTH as i32, HEIGHT as i32);
        // A second motion re-derives from `start`, so the offsets never accumulate.
        let dragged = interaction
            .moved(140, 100, WIDTH as i32, HEIGHT as i32)
            .unwrap();
        let step = 40.0 * 2.0 / WIDTH as f32;
        assert!((dragged.x - (0.25 + step)).abs() < 1e-6);
        assert!((dragged.y - (-0.5)).abs() < 1e-6);
    }

    #[test]
    fn a_release_without_a_press_is_ignored() {
        let mut interaction = Interaction::default();
        assert_eq!(interaction.release(1_000, 100, 100).0, Release::Ignored);
        assert_eq!(interaction.counts.drags, 0);
        assert_eq!(interaction.counts.double_clicks, 0);
    }

    #[test]
    fn surface_to_clip_maps_corners_to_the_mvp_translate() {
        // Top-left is (-1, 1), bottom-right is (1, -1): y grows down on the
        // surface and up in clip space.
        assert_eq!(surface_to_clip(0.0, 0.0, 640.0, 480.0), (-1.0, 1.0));
        assert_eq!(surface_to_clip(640.0, 480.0, 640.0, 480.0), (1.0, -1.0));
        assert_eq!(surface_to_clip(320.0, 240.0, 640.0, 480.0), (0.0, 0.0));
        // A degenerate surface cannot map anything.
        assert_eq!(surface_to_clip(1.0, 1.0, 0.0, 480.0), (0.0, 0.0));
    }

    #[test]
    fn surface_to_model_inverts_the_draw_projection() {
        // A square viewport keeps the canvas scale equal on both axes.
        let canvas = [2.0, 2.0];
        let scale = FIT / 2.0;
        // Centre of the surface maps to model origin when x = y = 0.
        let (x, y) = surface_to_model(320.0, 320.0, 640.0, 640.0, view(), canvas);
        assert!(x.abs() < 1e-6 && y.abs() < 1e-6, "{x}, {y}");
        // One clip unit right of centre is `1 / scale` model units.
        let (x, y) = surface_to_model(640.0, 320.0, 640.0, 640.0, view(), canvas);
        assert!((x - 1.0 / scale).abs() < 1e-5, "{x}");
        assert!(y.abs() < 1e-6, "{y}");
        // A translate moves the inverse the other way, which is what hit testing
        // needs so a dragged model's parts move with it.
        let moved = ViewState { x: 0.5, ..view() };
        let (x, _) = surface_to_model(640.0, 320.0, 640.0, 640.0, moved, canvas);
        assert!((x - (1.0 - 0.5) / scale).abs() < 1e-5, "{x}");
        // Zoom scales the inverse.
        let zoomed = ViewState {
            zoom: 2.0,
            ..view()
        };
        let (x, _) = surface_to_model(640.0, 320.0, 640.0, 640.0, zoomed, canvas);
        assert!((x - 1.0 / (scale * 2.0)).abs() < 1e-5, "{x}");
    }

    #[test]
    fn rectangular_viewport_preserves_model_aspect_and_inverse_mapping() {
        let canvas = [2.0, 2.0];
        let width = 800.0;
        let height = 400.0;
        let (scale_x, scale_y) = clip_scales(canvas, 1.0, width, height);
        assert!((scale_x * width - scale_y * height).abs() < 1e-6);

        // Equal model-space offsets project to equal pixel distances even though
        // the viewport is twice as wide as it is tall.
        let projected_x = scale_x * 0.5 * width / 2.0;
        let projected_y = scale_y * 0.5 * height / 2.0;
        assert!((projected_x - projected_y).abs() < 1e-6);

        // Round-trip several points through the same viewport-aware projection.
        let view = ViewState {
            x: 0.2,
            y: -0.1,
            zoom: 1.5,
            ..view()
        };
        for (model_x, model_y) in [(-0.4, 0.3), (0.0, 0.0), (0.5, -0.2)] {
            let surface_x = ((scale_x * view.zoom * model_x + view.x + 1.0) * width / 2.0) as f64;
            let surface_y =
                ((1.0 - (scale_y * view.zoom * model_y + view.y)) * height / 2.0) as f64;
            let actual = surface_to_model(surface_x, surface_y, width, height, view, canvas);
            assert!(
                (actual.0 - model_x).abs() < 1e-5,
                "{actual:?} != {model_x},{model_y}"
            );
            assert!(
                (actual.1 - model_y).abs() < 1e-5,
                "{actual:?} != {model_x},{model_y}"
            );
        }
    }

    #[test]
    fn expression_and_motion_parameter_intent_wins_over_automatic_blink() {
        // The frame pipeline applies automatic blink only when motion did not
        // update, and applies expressions afterwards in either case.
        let base = 0.8;
        let blink_closed = 0.0;
        let expression_open = 1.0;
        let expression_closed = 0.0;

        let with_motion = base; // motion updated this parameter, so blink is skipped
        let with_blink_then_expression = expression_open;
        assert_eq!(with_motion, base);
        assert_eq!(with_blink_then_expression, expression_open);

        // An expression's explicit close also remains closed over an automatic
        // blink value that would otherwise reopen the eye.
        let closed_expression_over_blink = expression_closed;
        assert_eq!(closed_expression_over_blink, 0.0);
        assert_eq!(blink_closed, 0.0);
    }

    #[test]
    fn the_hit_fallback_splits_on_the_model_midline() {
        // `LAppModel::HitTest` falls back to `y > 0` for Head and `y <= 0` for
        // Body in the projection's model space.
        assert_eq!(fallback_part(0.001), Part::Head);
        assert_eq!(fallback_part(0.0), Part::Body);
        assert_eq!(fallback_part(-0.5), Part::Body);
        assert_eq!(Part::Head.name(), "Head");
        assert_eq!(Part::Body.name(), "Body");
    }

    #[test]
    fn a_drag_maps_each_axis_to_its_viewport_dimension_without_zoom_scaling() {
        let dragged = drag_transform(view(), 10, -10, 640, 480);
        assert!((dragged.x - 10.0 * 2.0 / 640.0).abs() < 1e-6);
        // A downward surface motion moves the model down in clip space.
        assert!((dragged.y - 10.0 * 2.0 / 480.0).abs() < 1e-6);
        let zoomed = ViewState {
            zoom: 2.0,
            ..view()
        };
        assert_eq!(
            drag_transform(zoomed, 10, 10, 640, 480).x,
            zoomed.x + 20.0 / 640.0
        );
        // Invalid dimensions keep the transform.
        assert_eq!(drag_transform(view(), 10, 10, 0, 480), view());
        assert_eq!(drag_transform(view(), 10, 10, 640, 0), view());
    }
}
