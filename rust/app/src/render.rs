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
//! Scope: this module draws one model into one surface. Motion policy is the
//! single declared `Idle` group, if the model declares one; expressions, physics,
//! pose, look-at and the C++ transform editor are not implemented here.

use crate::Entry;
use cubism_render::{EyeBlink, Framework, Model as RenderModel, Motion, MotionManager, Renderer};
use deskwaifu_overlay::{Overlay, OverlayConfig};
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
/// `deskwaifu_model_domain::extract_input_regions`.
const VISIBLE_ALPHA: u8 = 10;
/// Margin of the canvas fit: the longer canvas side covers 90% of the surface.
const FIT: f32 = 1.8;
/// Group name of the only motion policy this runtime applies.
const IDLE_GROUP: &str = "Idle";

/// The display state the IPC layer owns, pushed into the next drawn frame.
#[derive(Clone, Copy, Debug)]
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
}

impl Stats {
    pub fn to_json(self, available: bool) -> Value {
        json!({
            "ok": true,
            "renderer_available": available,
            "frames_drawn": self.frames_drawn,
            "visible_pixels": self.visible_pixels,
            "transparent_pixels": self.transparent_pixels,
            "pixel_checksum": self.pixel_checksum,
            "width": self.width,
            "height": self.height,
        })
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
    model: RenderModel,
    /// Parameter values written through IPC, by parameter index. They are
    /// re-applied after the per-frame `LoadParameters`/`SaveParameters` layer
    /// restore, which would otherwise overwrite them with the base snapshot.
    overrides: Vec<(i32, f32)>,
    /// Canvas size cached from load; the draw path must not fail on it.
    canvas: [f32; 2],
}

/// The declared `Idle` motion, kept alive so a finished playback can restart.
struct Playing {
    manager: MotionManager,
    motion: Motion,
}

/// Rust-owned GL texture names; deleted only while the context is current.
struct Textures(Vec<u32>);

impl Drop for Textures {
    fn drop(&mut self) {
        if !self.0.is_empty() {
            unsafe { gl::DeleteTextures(self.0.len() as i32, self.0.as_ptr()) };
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
        let mut textures = Textures(Vec::new());
        for name in entry.model3.texture_file_names() {
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
            textures.0.push(texture);
        }
        let (width, height) = (size.0 as i32, size.1 as i32);
        let mut renderer = model
            .renderer(width, height)
            .map_err(|e| format!("cannot create renderer: {e}"))?;
        for (slot, &texture) in textures.0.iter().enumerate() {
            renderer
                .bind_texture(slot as i32, texture)
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
        Ok(Scene {
            model,
            _textures: textures,
            renderer,
            motion,
            eye_blink,
            overrides: Vec::new(),
            canvas,
        })
    }

    /// One frame: the SDK layer order documented in `cubism-render`'s README.
    fn update(&mut self, delta: f32) -> Result<(), String> {
        let Scene {
            model,
            motion,
            eye_blink,
            overrides,
            ..
        } = self;
        model
            .load_parameters()
            .map_err(|e| format!("cannot load parameters: {e}"))?;
        if let Some(playing) = motion {
            let updated = playing
                .manager
                .update(delta)
                .map_err(|e| format!("motion update failed: {e}"))?;
            // A finished non-looping motion is restarted; a looping motion never
            // reports finished, so this only affects one-shot idle files.
            if !updated && playing.manager.is_finished().unwrap_or(false) {
                playing
                    .manager
                    .start(&playing.motion, 0)
                    .map_err(|e| format!("cannot restart idle motion: {e}"))?;
            }
        }
        if let Some(blink) = eye_blink {
            blink
                .update(delta)
                .map_err(|e| format!("eye blink update failed: {e}"))?;
        }
        model
            .save_parameters()
            .map_err(|e| format!("cannot save parameters: {e}"))?;
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
    fn mvp(&self, view: ViewState) -> [f32; 16] {
        let longest = self.canvas[0].max(self.canvas[1]).max(f32::MIN_POSITIVE);
        let scale = FIT / longest * view.zoom;
        [
            scale, 0., 0., 0., 0., scale, 0., 0., 0., 0., 1., 0., view.x, view.y, 0., 1.,
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
        // Honest first step: the whole surface is click-through. The C++ window
        // derives a shaped region from the rendered alpha
        // (`deskwaifu_model_domain::extract_input_regions` over a readback, then
        // `frame_input_regions`), which needs the alpha analysis and screen-edit
        // mode this runtime has no command for yet.
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
                self.scene = None;
                self.error = Some(error.clone());
                Err(error)
            }
        }
    }

    /// Applies one parameter to the rendered model by parameter ID.
    pub fn set_parameter(&mut self, id: &str, value: f32) -> Result<(), String> {
        if self.scene.is_none() {
            return Err(self.unavailable());
        }
        let scene = self.scene.as_mut().expect("checked above");
        scene.set_parameter(id, value)
    }

    /// The next 16 ms frame: update, draw, present, then read back only on the
    /// first frame so pixel counts exist without a per-frame download.
    pub fn frame(&mut self, view: ViewState) {
        if self.scene.is_none() {
            return;
        }
        let readback = self.frames_drawn == 0;
        if let Err(error) = self.draw(view, readback) {
            self.fail(error);
        }
    }

    /// Forces one frame and a fresh readback, for diagnostics and the live test.
    pub fn stats(&mut self, view: ViewState) -> Result<Stats, String> {
        if self.scene.is_none() {
            return Err(self.unavailable());
        }
        self.draw(view, true)?;
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

    fn draw(&mut self, view: ViewState, readback: bool) -> Result<(), String> {
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
        let mvp = scene.mvp(view);
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
        if readback {
            self.stats = Some(self.readback(width, height)?);
        }
        self.overlay
            .swap_buffers()
            .map_err(|e| format!("cannot present the frame: {e}"))?;
        self.overlay
            .dispatch_pending()
            .map_err(|e| format!("cannot dispatch Wayland events: {e}"))
    }

    fn readback(&mut self, width: i32, height: i32) -> Result<Stats, String> {
        let mut pixels = vec![0u8; (width * height * 4) as usize];
        unsafe {
            gl::ReadPixels(
                0,
                0,
                width,
                height,
                gl::RGBA,
                gl::UNSIGNED_BYTE,
                pixels.as_mut_ptr().cast(),
            );
        }
        let error = unsafe { gl::GetError() };
        if error != gl::NO_ERROR {
            return Err(format!("GL readback failed (GL error {error:#x})"));
        }
        let mut visible_pixels = 0;
        let mut transparent_pixels = 0;
        let mut pixel_checksum = 0xcbf2_9ce4_8422_2325u64;
        for pixel in pixels.as_chunks::<4>().0 {
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
        Ok(Stats {
            frames_drawn: self.frames_drawn,
            visible_pixels,
            transparent_pixels,
            pixel_checksum,
            width: width as u32,
            height: height as u32,
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
