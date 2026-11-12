//! Minimal Cubism Framework/OpenGL rendering facade. See the README for context
//! lifetime requirements and external SDK licensing.
//!
//! Motion, expression and automatic eye-blink primitives mirror the SDK
//! manager APIs; this crate applies no playback policy of its own. The caller
//! picks groups, priorities, fades and blink parameter IDs.
use std::{
    ffi::{c_char, c_int, c_void, CStr, CString},
    marker::PhantomData,
    path::Path,
    ptr::NonNull,
    rc::Rc,
};

mod ffi {
    use super::*;
    extern "C" {
        pub fn cr_error() -> *const c_char;
        pub fn cr_start(directory: *const c_char) -> c_int;
        pub fn cr_stop() -> c_int;
        pub fn cr_model_create(bytes: *const u8, size: usize, out: *mut *mut c_void) -> c_int;
        pub fn cr_model_destroy(model: *mut c_void) -> c_int;
        pub fn cr_model_update(model: *mut c_void) -> c_int;
        pub fn cr_model_load_parameters(model: *mut c_void) -> c_int;
        pub fn cr_model_save_parameters(model: *mut c_void) -> c_int;
        pub fn cr_model_canvas(model: *mut c_void, w: *mut f32, h: *mut f32) -> c_int;
        pub fn cr_parameter_count(model: *mut c_void, count: *mut c_int) -> c_int;
        pub fn cr_parameter_get(
            model: *mut c_void,
            index: c_int,
            id: *mut *const c_char,
            value: *mut f32,
            min: *mut f32,
            max: *mut f32,
            initial: *mut f32,
        ) -> c_int;
        pub fn cr_parameter_set(model: *mut c_void, index: c_int, value: f32) -> c_int;
        pub fn cr_renderer_create(
            model: *mut c_void,
            w: c_int,
            h: c_int,
            out: *mut *mut c_void,
        ) -> c_int;
        pub fn cr_renderer_destroy(renderer: *mut c_void) -> c_int;
        pub fn cr_renderer_bind_texture(renderer: *mut c_void, slot: c_int, texture: u32) -> c_int;
        pub fn cr_renderer_resize(renderer: *mut c_void, w: c_int, h: c_int) -> c_int;
        pub fn cr_renderer_draw(renderer: *mut c_void, matrix: *const f32) -> c_int;
        pub fn cr_motion_create(bytes: *const u8, size: usize, out: *mut *mut c_void) -> c_int;
        pub fn cr_motion_destroy(motion: *mut c_void) -> c_int;
        pub fn cr_motion_set_fade_in(motion: *mut c_void, seconds: f32) -> c_int;
        pub fn cr_motion_set_fade_out(motion: *mut c_void, seconds: f32) -> c_int;
        pub fn cr_motion_manager_create(model: *mut c_void, out: *mut *mut c_void) -> c_int;
        pub fn cr_motion_manager_destroy(manager: *mut c_void) -> c_int;
        pub fn cr_motion_manager_start(
            manager: *mut c_void,
            motion: *mut c_void,
            priority: c_int,
            out_entry: *mut u64,
        ) -> c_int;
        pub fn cr_motion_manager_update(
            manager: *mut c_void,
            delta_seconds: f32,
            out_updated: *mut c_int,
        ) -> c_int;
        pub fn cr_motion_manager_stop(manager: *mut c_void) -> c_int;
        pub fn cr_motion_manager_is_finished(
            manager: *mut c_void,
            out_finished: *mut c_int,
        ) -> c_int;
        pub fn cr_motion_manager_entry_is_finished(
            manager: *mut c_void,
            entry: u64,
            out_finished: *mut c_int,
        ) -> c_int;
        pub fn cr_expression_create(bytes: *const u8, size: usize, out: *mut *mut c_void) -> c_int;
        pub fn cr_expression_destroy(expression: *mut c_void) -> c_int;
        pub fn cr_expression_manager_create(model: *mut c_void, out: *mut *mut c_void) -> c_int;
        pub fn cr_expression_manager_destroy(manager: *mut c_void) -> c_int;
        pub fn cr_expression_manager_start(manager: *mut c_void, expression: *mut c_void) -> c_int;
        pub fn cr_expression_manager_update(
            manager: *mut c_void,
            delta_seconds: f32,
            out_updated: *mut c_int,
        ) -> c_int;
        pub fn cr_eye_blink_create(
            model: *mut c_void,
            ids: *const *const c_char,
            count: c_int,
            out: *mut *mut c_void,
        ) -> c_int;
        pub fn cr_eye_blink_destroy(eye_blink: *mut c_void) -> c_int;
        pub fn cr_eye_blink_set_interval(eye_blink: *mut c_void, seconds: f32) -> c_int;
        pub fn cr_eye_blink_set_settings(
            eye_blink: *mut c_void,
            closing: f32,
            closed: f32,
            opening: f32,
        ) -> c_int;
        pub fn cr_eye_blink_update(
            eye_blink: *mut c_void,
            model: *mut c_void,
            delta_seconds: f32,
        ) -> c_int;
    }
}

#[derive(Debug, Clone)]
pub struct Error(pub String);
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        self.0.fmt(f)
    }
}
impl std::error::Error for Error {}
pub type Result<T> = std::result::Result<T, Error>;
fn check(code: c_int) -> Result<()> {
    if code == 0 {
        Ok(())
    } else {
        Err(Error(unsafe {
            CStr::from_ptr(ffi::cr_error())
                .to_string_lossy()
                .into_owned()
        }))
    }
}
/// Rejects invalid time steps before they reach the native manager.
fn delta(delta_seconds: f32) -> Result<f32> {
    if delta_seconds.is_finite() && delta_seconds >= 0. {
        Ok(delta_seconds)
    } else {
        Err(Error(
            "delta seconds must be finite and non-negative".into(),
        ))
    }
}
fn non_empty(bytes: &[u8], what: &str) -> Result<()> {
    if bytes.is_empty() {
        Err(Error(format!("{what} bytes required")))
    } else {
        Ok(())
    }
}

/// A process-global Framework lease. All handles are `!Send` and `!Sync`.
/// Models retain this lease; renderers in turn retain their model.
#[derive(Clone)]
pub struct Framework(Rc<FrameworkInner>);
struct FrameworkInner;
impl Framework {
    /// Starts the Framework with shaders from the SDK's external `Standard` directory.
    /// A second simultaneous session is rejected, including on other threads.
    ///
    /// # Safety
    /// An EGL desktop OpenGL compatibility context must be current. The caller
    /// must keep that same context current on this thread through the destruction
    /// of **all** clones, models and renderers. Do not initialize/dispose Cubism
    /// outside this crate while this lease exists. GL textures must not be deleted
    /// or repurposed while bound to a renderer that may draw them.
    pub unsafe fn start(standard_shader_directory: &Path) -> Result<Self> {
        use std::os::unix::ffi::OsStrExt;
        let path = CString::new(standard_shader_directory.as_os_str().as_bytes())
            .map_err(|_| Error("shader directory contains NUL".into()))?;
        check(ffi::cr_start(path.as_ptr()))?;
        Ok(Self(Rc::new(FrameworkInner)))
    }

    pub fn create_model(&self, moc: &[u8]) -> Result<Model> {
        let mut raw = std::ptr::null_mut();
        check(unsafe { ffi::cr_model_create(moc.as_ptr(), moc.len(), &mut raw) })?;
        Ok(Model(Rc::new(ModelInner {
            raw: NonNull::new(raw).expect("successful native model"),
            _framework: self.clone(),
        })))
    }

    /// Parses external `.motion3.json` bytes. The motion is inert until a
    /// [`MotionManager`] starts it, and the caller owns it until drop.
    pub fn create_motion(&self, json: &[u8]) -> Result<Motion> {
        non_empty(json, "motion3.json")?;
        let mut raw = std::ptr::null_mut();
        check(unsafe { ffi::cr_motion_create(json.as_ptr(), json.len(), &mut raw) })?;
        Ok(Motion {
            raw: NonNull::new(raw).expect("successful native motion"),
            _framework: self.clone(),
            _not_send_sync: PhantomData,
        })
    }

    /// Parses external `.exp3.json` bytes. The expression is inert until an
    /// [`ExpressionManager`] starts it, and the caller owns it until drop.
    pub fn create_expression(&self, json: &[u8]) -> Result<Expression> {
        non_empty(json, "exp3.json")?;
        let mut raw = std::ptr::null_mut();
        check(unsafe { ffi::cr_expression_create(json.as_ptr(), json.len(), &mut raw) })?;
        Ok(Expression {
            raw: NonNull::new(raw).expect("successful native expression"),
            _framework: self.clone(),
            _not_send_sync: PhantomData,
        })
    }

    /// Explicit shutdown succeeds only after all dependent handles/clones drop.
    /// On failure the returned lease remains usable.
    pub fn close(self) -> std::result::Result<(), Self> {
        if Rc::strong_count(&self.0) != 1 {
            return Err(self);
        }
        drop(self);
        Ok(())
    }
}
impl Drop for FrameworkInner {
    fn drop(&mut self) {
        // Violating the unsafe start contract must not free GL resources in the
        // wrong context. Native code refuses shutdown instead (leaking safely).
        let _ = unsafe { ffi::cr_stop() };
    }
}

#[derive(Clone)]
pub struct Model(Rc<ModelInner>);
struct ModelInner {
    raw: NonNull<c_void>,
    _framework: Framework,
}
impl Drop for ModelInner {
    fn drop(&mut self) {
        let _ = unsafe { ffi::cr_model_destroy(self.raw.as_ptr()) };
    }
}
#[derive(Debug, Clone)]
pub struct Parameter {
    pub id: String,
    pub value: f32,
    pub minimum: f32,
    pub maximum: f32,
    pub default: f32,
}
impl Model {
    pub fn canvas_size(&self) -> Result<[f32; 2]> {
        let (mut w, mut h) = (0., 0.);
        check(unsafe { ffi::cr_model_canvas(self.0.raw.as_ptr(), &mut w, &mut h) })?;
        Ok([w, h])
    }
    pub fn parameter_count(&self) -> Result<i32> {
        let mut count = 0;
        check(unsafe { ffi::cr_parameter_count(self.0.raw.as_ptr(), &mut count) })?;
        Ok(count)
    }
    pub fn parameter(&self, index: i32) -> Result<Parameter> {
        let mut id = std::ptr::null();
        let (mut value, mut minimum, mut maximum, mut default) = (0., 0., 0., 0.);
        check(unsafe {
            ffi::cr_parameter_get(
                self.0.raw.as_ptr(),
                index,
                &mut id,
                &mut value,
                &mut minimum,
                &mut maximum,
                &mut default,
            )
        })?;
        Ok(Parameter {
            id: unsafe { CStr::from_ptr(id) }.to_string_lossy().into_owned(),
            value,
            minimum,
            maximum,
            default,
        })
    }
    pub fn set_parameter(&self, index: i32, value: f32) -> Result<()> {
        check(unsafe { ffi::cr_parameter_set(self.0.raw.as_ptr(), index, value) })
    }
    pub fn update(&self) -> Result<()> {
        check(unsafe { ffi::cr_model_update(self.0.raw.as_ptr()) })
    }
    /// Restores the parameter values captured by the last [`Self::save_parameters`].
    pub fn load_parameters(&self) -> Result<()> {
        check(unsafe { ffi::cr_model_load_parameters(self.0.raw.as_ptr()) })
    }
    /// Captures the current parameter values as the base layer for motion blending.
    pub fn save_parameters(&self) -> Result<()> {
        check(unsafe { ffi::cr_model_save_parameters(self.0.raw.as_ptr()) })
    }
    pub fn renderer(&self, width: i32, height: i32) -> Result<Renderer> {
        let mut raw = std::ptr::null_mut();
        check(unsafe { ffi::cr_renderer_create(self.0.raw.as_ptr(), width, height, &mut raw) })?;
        Ok(Renderer {
            raw: NonNull::new(raw).expect("successful native renderer"),
            _model: self.clone(),
        })
    }
    /// Creates a motion manager bound to this model. The manager keeps the model
    /// lease and its own reference to every started motion until stop/drop.
    pub fn motion_manager(&self) -> Result<MotionManager> {
        let mut raw = std::ptr::null_mut();
        check(unsafe { ffi::cr_motion_manager_create(self.0.raw.as_ptr(), &mut raw) })?;
        Ok(MotionManager {
            raw: NonNull::new(raw).expect("successful native motion manager"),
            _model: self.clone(),
            _not_send_sync: PhantomData,
        })
    }
    /// Creates an expression manager bound to this model.
    pub fn expression_manager(&self) -> Result<ExpressionManager> {
        let mut raw = std::ptr::null_mut();
        check(unsafe { ffi::cr_expression_manager_create(self.0.raw.as_ptr(), &mut raw) })?;
        Ok(ExpressionManager {
            raw: NonNull::new(raw).expect("successful native expression manager"),
            _model: self.clone(),
            _not_send_sync: PhantomData,
        })
    }
    /// Creates an automatic eye blink driver for explicit model parameter IDs.
    /// Every ID must exist on this model; nothing is guessed from model settings.
    pub fn eye_blink(&self, parameter_ids: &[&str]) -> Result<EyeBlink> {
        if parameter_ids.is_empty() {
            return Err(Error("eye blink requires at least one parameter ID".into()));
        }
        let ids = parameter_ids
            .iter()
            .map(|id| CString::new(*id).map_err(|_| Error("parameter ID contains NUL".into())))
            .collect::<Result<Vec<_>>>()?;
        let pointers = ids.iter().map(|id| id.as_ptr()).collect::<Vec<_>>();
        let mut raw = std::ptr::null_mut();
        check(unsafe {
            ffi::cr_eye_blink_create(
                self.0.raw.as_ptr(),
                pointers.as_ptr(),
                pointers.len() as c_int,
                &mut raw,
            )
        })?;
        Ok(EyeBlink {
            raw: NonNull::new(raw).expect("successful native eye blink"),
            _model: self.clone(),
            _not_send_sync: PhantomData,
        })
    }
}
/// Owns native renderer state, not the caller's GL textures or render target.
pub struct Renderer {
    raw: NonNull<c_void>,
    _model: Model,
}
impl Renderer {
    pub fn bind_texture(&mut self, slot: i32, texture: u32) -> Result<()> {
        check(unsafe { ffi::cr_renderer_bind_texture(self.raw.as_ptr(), slot, texture) })
    }
    /// Update internal target size; caller owns viewport and framebuffer resize.
    pub fn resize(&mut self, width: i32, height: i32) -> Result<()> {
        check(unsafe { ffi::cr_renderer_resize(self.raw.as_ptr(), width, height) })
    }
    /// Draws to the currently bound framebuffer using a column-major MVP matrix.
    pub fn draw(&mut self, matrix: &[f32; 16]) -> Result<()> {
        check(unsafe { ffi::cr_renderer_draw(self.raw.as_ptr(), matrix.as_ptr()) })
    }
}
impl Drop for Renderer {
    fn drop(&mut self) {
        let _ = unsafe { ffi::cr_renderer_destroy(self.raw.as_ptr()) };
    }
}

/// An externally loaded motion. `!Send`/`!Sync`; must not outlive its Framework.
///
/// Dropping the handle releases only the handle: a playing manager keeps its own
/// reference until the entry finishes, is stopped, or the manager drops.
pub struct Motion {
    raw: NonNull<c_void>,
    _framework: Framework,
    _not_send_sync: PhantomData<*mut ()>,
}
impl Motion {
    /// Sets the fade-in duration in seconds. The caller decides the value.
    pub fn set_fade_in(&mut self, seconds: f32) -> Result<()> {
        check(unsafe { ffi::cr_motion_set_fade_in(self.raw.as_ptr(), seconds) })
    }
    /// Sets the fade-out duration in seconds. The caller decides the value.
    pub fn set_fade_out(&mut self, seconds: f32) -> Result<()> {
        check(unsafe { ffi::cr_motion_set_fade_out(self.raw.as_ptr(), seconds) })
    }
}
impl Drop for Motion {
    fn drop(&mut self) {
        let _ = unsafe { ffi::cr_motion_destroy(self.raw.as_ptr()) };
    }
}

/// An externally loaded expression. `!Send`/`!Sync`.
pub struct Expression {
    raw: NonNull<c_void>,
    _framework: Framework,
    _not_send_sync: PhantomData<*mut ()>,
}
impl Drop for Expression {
    fn drop(&mut self) {
        let _ = unsafe { ffi::cr_expression_destroy(self.raw.as_ptr()) };
    }
}

/// Opaque token for one started motion, used to query its finish state.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MotionEntry(u64);

/// Owns SDK motion queue state for one model. Retains the model lease.
pub struct MotionManager {
    raw: NonNull<c_void>,
    _model: Model,
    _not_send_sync: PhantomData<*mut ()>,
}
impl MotionManager {
    /// Starts a motion with an explicit priority. No fallback/idle policy is applied.
    pub fn start(&mut self, motion: &Motion, priority: i32) -> Result<MotionEntry> {
        let mut entry = 0;
        check(unsafe {
            ffi::cr_motion_manager_start(
                self.raw.as_ptr(),
                motion.raw.as_ptr(),
                priority,
                &mut entry,
            )
        })?;
        Ok(MotionEntry(entry))
    }
    /// Steps the queue. The caller owns the clock and the frame order.
    pub fn update(&mut self, delta_seconds: f32) -> Result<bool> {
        let delta = delta(delta_seconds)?;
        let mut updated = 0;
        check(unsafe { ffi::cr_motion_manager_update(self.raw.as_ptr(), delta, &mut updated) })?;
        Ok(updated != 0)
    }
    pub fn stop(&mut self) -> Result<()> {
        check(unsafe { ffi::cr_motion_manager_stop(self.raw.as_ptr()) })
    }
    pub fn is_finished(&self) -> Result<bool> {
        let mut finished = 0;
        check(unsafe { ffi::cr_motion_manager_is_finished(self.raw.as_ptr(), &mut finished) })?;
        Ok(finished != 0)
    }
    /// Reports whether one started motion finished (or its entry is gone).
    pub fn entry_is_finished(&self, entry: MotionEntry) -> Result<bool> {
        let mut finished = 0;
        check(unsafe {
            ffi::cr_motion_manager_entry_is_finished(self.raw.as_ptr(), entry.0, &mut finished)
        })?;
        Ok(finished != 0)
    }
}
impl Drop for MotionManager {
    fn drop(&mut self) {
        let _ = unsafe { ffi::cr_motion_manager_destroy(self.raw.as_ptr()) };
    }
}

/// Owns SDK expression blending state for one model. Retains the model lease.
pub struct ExpressionManager {
    raw: NonNull<c_void>,
    _model: Model,
    _not_send_sync: PhantomData<*mut ()>,
}
impl ExpressionManager {
    /// Starts an expression; blends with the previous one using its own fades.
    pub fn start(&mut self, expression: &Expression) -> Result<()> {
        check(unsafe {
            ffi::cr_expression_manager_start(self.raw.as_ptr(), expression.raw.as_ptr())
        })
    }
    pub fn update(&mut self, delta_seconds: f32) -> Result<bool> {
        let delta = delta(delta_seconds)?;
        let mut updated = 0;
        check(unsafe {
            ffi::cr_expression_manager_update(self.raw.as_ptr(), delta, &mut updated)
        })?;
        Ok(updated != 0)
    }
}
impl Drop for ExpressionManager {
    fn drop(&mut self) {
        let _ = unsafe { ffi::cr_expression_manager_destroy(self.raw.as_ptr()) };
    }
}

/// Automatic eye blink driven only by the caller's explicit parameter IDs.
pub struct EyeBlink {
    raw: NonNull<c_void>,
    _model: Model,
    _not_send_sync: PhantomData<*mut ()>,
}
impl EyeBlink {
    /// Sets the mean blink interval in seconds; the caller chooses the cadence.
    pub fn set_interval(&mut self, seconds: f32) -> Result<()> {
        check(unsafe { ffi::cr_eye_blink_set_interval(self.raw.as_ptr(), seconds) })
    }
    /// Sets the closing, closed and opening phase durations in seconds.
    pub fn set_settings(&mut self, closing: f32, closed: f32, opening: f32) -> Result<()> {
        check(unsafe {
            ffi::cr_eye_blink_set_settings(self.raw.as_ptr(), closing, closed, opening)
        })
    }
    /// Advances the blink state machine and writes eyelid parameters on the model.
    pub fn update(&mut self, delta_seconds: f32) -> Result<()> {
        check(unsafe {
            ffi::cr_eye_blink_update(
                self.raw.as_ptr(),
                self._model.0.raw.as_ptr(),
                delta(delta_seconds)?,
            )
        })
    }
}
impl Drop for EyeBlink {
    fn drop(&mut self) {
        let _ = unsafe { ffi::cr_eye_blink_destroy(self.raw.as_ptr()) };
    }
}
