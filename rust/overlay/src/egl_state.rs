//! EGL context and window-surface lifecycle for one layer-shell surface.
//!
//! The caller owns every GL call; this module owns the EGL display, config,
//! context and window surface, plus the `wl_egl_window` that connects them to the
//! `wl_surface`.

use std::ffi::c_void;
use std::marker::PhantomData;

use khronos_egl as egl;

use crate::error::Error;

#[link(name = "wayland-egl")]
extern "C" {
    fn wl_egl_window_create(surface: *mut c_void, width: i32, height: i32) -> *mut c_void;
    fn wl_egl_window_destroy(window: *mut c_void);
    fn wl_egl_window_resize(window: *mut c_void, width: i32, height: i32, dx: i32, dy: i32);
}

/// A transparent-capable EGL window surface for a Wayland surface.
pub(crate) struct Egl {
    instance: egl::Instance<egl::Static>,
    display: egl::Display,
    context: egl::Context,
    surface: egl::Surface,
    window: *mut c_void,
    size: (u32, u32),
    /// Raw handles make the type `!Send`/`!Sync`; this keeps that explicit.
    not_send: PhantomData<*mut ()>,
}

impl Egl {
    /// Create an EGL context and window surface for `wl_surface`.
    ///
    /// The chosen config requests an alpha channel of at least eight bits, so the
    /// caller can clear with alpha 0 and get a fully transparent surface.
    pub(crate) fn new(
        display_ptr: *mut c_void,
        wl_surface: *mut c_void,
        size: (u32, u32),
    ) -> Result<Egl, Error> {
        if size.0 == 0 || size.1 == 0 {
            return Err(Error::InvalidSize);
        }
        let instance = egl::Instance::new(egl::Static);
        // SAFETY: `display_ptr` is the live `wl_display` of the connection that
        // owns `wl_surface`; the caller guarantees it outlives the EGL state.
        let display = unsafe { instance.get_display(display_ptr) }.ok_or(egl::Error::BadDisplay)?;
        instance.initialize(display)?;
        instance.bind_api(egl::OPENGL_API)?;

        let attributes = [
            egl::SURFACE_TYPE,
            egl::WINDOW_BIT,
            egl::RENDERABLE_TYPE,
            egl::OPENGL_BIT,
            egl::RED_SIZE,
            8,
            egl::GREEN_SIZE,
            8,
            egl::BLUE_SIZE,
            8,
            egl::ALPHA_SIZE,
            8,
            egl::NONE,
        ];
        let config = instance
            .choose_first_config(display, &attributes)?
            .ok_or(egl::Error::BadConfig)?;

        let window = unsafe {
            wl_egl_window_create(
                wl_surface,
                i32::try_from(size.0).map_err(|_| Error::InvalidSize)?,
                i32::try_from(size.1).map_err(|_| Error::InvalidSize)?,
            )
        };
        if window.is_null() {
            return Err(Error::Egl(egl::Error::BadAlloc));
        }

        // SAFETY: `window` was just created for this display and config.
        let surface =
            match unsafe { instance.create_window_surface(display, config, window.cast(), None) } {
                Ok(surface) => surface,
                Err(error) => {
                    unsafe { wl_egl_window_destroy(window) };
                    return Err(Error::Egl(error));
                }
            };

        let context = match instance.create_context(display, config, None, &[egl::NONE]) {
            Ok(context) => context,
            Err(error) => {
                let _ = instance.destroy_surface(display, surface);
                unsafe { wl_egl_window_destroy(window) };
                return Err(Error::Egl(error));
            }
        };

        Ok(Egl {
            instance,
            display,
            context,
            surface,
            window,
            size,
            not_send: PhantomData,
        })
    }

    pub(crate) fn make_current(&self) -> Result<(), Error> {
        self.instance.make_current(
            self.display,
            Some(self.surface),
            Some(self.surface),
            Some(self.context),
        )?;
        Ok(())
    }

    pub(crate) fn swap_buffers(&self) -> Result<(), Error> {
        self.instance.swap_buffers(self.display, self.surface)?;
        Ok(())
    }

    pub(crate) fn resize(&mut self, size: (u32, u32)) -> Result<(), Error> {
        if size == self.size || size.0 == 0 || size.1 == 0 {
            return Ok(());
        }
        unsafe {
            wl_egl_window_resize(
                self.window,
                i32::try_from(size.0).map_err(|_| Error::InvalidSize)?,
                i32::try_from(size.1).map_err(|_| Error::InvalidSize)?,
                0,
                0,
            );
        }
        self.size = size;
        Ok(())
    }

    pub(crate) fn size(&self) -> (u32, u32) {
        self.size
    }

    pub(crate) fn display(&self) -> egl::Display {
        self.display
    }

    pub(crate) fn instance(&self) -> &egl::Instance<egl::Static> {
        &self.instance
    }
}

impl Drop for Egl {
    fn drop(&mut self) {
        let _ = self.instance.make_current(self.display, None, None, None);
        let _ = self.instance.destroy_context(self.display, self.context);
        let _ = self.instance.destroy_surface(self.display, self.surface);
        unsafe { wl_egl_window_destroy(self.window) };
        // The display is owned by this crate's connection, so terminate it here.
        let _ = self.instance.terminate(self.display);
    }
}
