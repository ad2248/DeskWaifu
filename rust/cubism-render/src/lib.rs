//! Minimal Cubism Framework/OpenGL rendering facade. See the README for context
//! lifetime requirements and external SDK licensing.
use std::{
    ffi::{c_char, c_int, c_void, CStr, CString},
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
    pub fn renderer(&self, width: i32, height: i32) -> Result<Renderer> {
        let mut raw = std::ptr::null_mut();
        check(unsafe { ffi::cr_renderer_create(self.0.raw.as_ptr(), width, height, &mut raw) })?;
        Ok(Renderer {
            raw: NonNull::new(raw).expect("successful native renderer"),
            _model: self.clone(),
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
