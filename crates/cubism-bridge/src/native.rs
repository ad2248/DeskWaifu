use std::{
    ffi::{CStr, OsStr, c_char, c_void},
    fmt,
    marker::PhantomData,
    os::unix::ffi::OsStrExt,
    path::{Path, PathBuf},
    ptr::{self, NonNull},
    rc::Rc,
};

type DeviceVisitor = extern "C" fn(*mut c_void, *const c_char, *const c_char);

unsafe extern "C" {
    fn dw_error() -> *const c_char;
    fn dw_create(width: i32, height: i32, out: *mut *mut c_void) -> i32;
    fn dw_devices(visitor: DeviceVisitor, data: *mut c_void) -> i32;
    fn dw_active_device(runtime: *const c_void, visitor: DeviceVisitor, data: *mut c_void);
    fn dw_create_on_device(
        width: i32,
        height: i32,
        by_pci: i32,
        selector: *const u8,
        length: usize,
        out: *mut *mut c_void,
    ) -> i32;
    fn dw_create_wayland(
        width: i32,
        height: i32,
        display: *mut c_void,
        window: *mut c_void,
        out: *mut *mut c_void,
    ) -> i32;
    fn dw_draw_window(model: *mut c_void, dt: f32) -> i32;
    fn dw_read_window(runtime: *mut c_void, sampled: i32, pixels: *mut u8, length: usize) -> i32;
    fn dw_present_window(runtime: *mut c_void) -> i32;
    fn dw_renderer(runtime: *const c_void) -> *const c_char;
    fn dw_load(runtime: *mut c_void, path: *const u8, length: usize, out: *mut *mut c_void) -> i32;
    fn dw_uses_masking(model: *const c_void) -> i32;
    fn dw_metadata_count(model: *mut c_void, expressions: i32) -> i32;
    fn dw_metadata_entry(
        model: *mut c_void,
        expressions: i32,
        index: i32,
        name: *mut *const c_char,
        motions: *mut i32,
    ) -> i32;
    fn dw_expression(model: *mut c_void, name: *const u8, length: usize) -> i32;
    fn dw_set_expression_timeout(model: *mut c_void, timeout: f32) -> i32;
    fn dw_toggle_expression(
        model: *mut c_void,
        file: *const u8,
        length: usize,
        fade: f32,
        duration: f32,
        active: *mut i32,
    ) -> i32;
    fn dw_expression_toggle_active(
        model: *const c_void,
        file: *const u8,
        length: usize,
        active: *mut i32,
    ) -> i32;
    fn dw_reset_expression_toggles(model: *mut c_void);
    fn dw_motion(model: *mut c_void, group: *const u8, length: usize, index: i32) -> i32;
    fn dw_switch_skin(model: *mut c_void) -> i32;
    fn dw_tap(model: *mut c_void, x: f32, y: f32, hit: *mut i32) -> i32;
    fn dw_set_lip_sync(model: *mut c_void, active: i32, opening: f32) -> i32;
    fn dw_set_look(model: *mut c_void, active: i32, x: f32, y: f32) -> i32;
    fn dw_frame(model: *mut c_void, dt: f32, pixels: *mut u8, length: usize) -> i32;
    fn dw_destroy(runtime: *mut c_void);
    fn dw_model_destroy(model: *mut c_void);
}

#[derive(Debug)]
pub struct Error(String);
impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}
impl std::error::Error for Error {}

/// The legacy action area selected by a model tap.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TapHit {
    Miss,
    Head,
    Body,
}

fn check(status: i32) -> Result<(), Error> {
    if status == 0 {
        return Ok(());
    }
    // The shim returns a NUL-terminated thread-local string, copied before another call.
    let message = unsafe { CStr::from_ptr(dw_error()) }
        .to_string_lossy()
        .into_owned();
    Err(Error(message))
}

/// A device identity reported by EGL. Software or other non-DRM devices may have neither field.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RenderDevice {
    /// Absolute DRM render-node path, when supplied by the EGL device.
    pub render_node: Option<PathBuf>,
    /// Lowercase PCI address (`dddd:bb:ss.f`), when the DRM node maps to a PCI device.
    pub pci: Option<String>,
}

impl RenderDevice {
    /// Copy the optional NUL-terminated fields during the native visitor callback.
    unsafe fn copy_from_egl(render_node: *const c_char, pci: *const c_char) -> Self {
        Self {
            render_node: (!render_node.is_null()).then(|| {
                PathBuf::from(OsStr::from_bytes(
                    unsafe { CStr::from_ptr(render_node) }.to_bytes(),
                ))
            }),
            pci: (!pci.is_null()).then(|| {
                unsafe { CStr::from_ptr(pci) }
                    .to_string_lossy()
                    .into_owned()
            }),
        }
    }
}

/// Select a specific EGL device for offscreen rendering.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum DeviceSelector<'a> {
    /// An absolute DRM render-node path. Symlinks such as `/dev/dri/by-path/...` are accepted.
    RenderNode(&'a Path),
    /// A PCI address of the form `dddd:bb:ss.f`. Hex digits are case-insensitive.
    Pci(&'a str),
}

/// Owns one EGL context and the process-global Cubism Framework instance.
/// Construct, use and drop this object on a dedicated thread.
pub struct Runtime {
    raw: NonNull<c_void>,
    frame_len: usize,
    frame_size: (u32, u32),
    _thread: PhantomData<Rc<()>>,
}

/// A loaded model whose textures and Cubism objects are owned by the render thread.
pub struct Model<'runtime> {
    raw: NonNull<c_void>,
    runtime: &'runtime Runtime,
}

impl Runtime {
    pub fn new(width: u32, height: u32) -> Result<Self, Error> {
        // The native runtime owns a surfaceless EGL context.
        Self::create(width, height, |out| unsafe {
            dw_create(width as i32, height as i32, out)
        })
    }

    /// Enumerate devices advertised by EGL without initializing a rendering context.
    /// Returns an error when the EGL implementation lacks device enumeration.
    pub fn devices() -> Result<Vec<RenderDevice>, Error> {
        extern "C" fn collect(data: *mut c_void, node: *const c_char, pci: *const c_char) {
            // `dw_devices` visits synchronously, with live borrowed strings and the original data pointer.
            unsafe { &mut *data.cast::<Vec<RenderDevice>>() }
                .push(unsafe { RenderDevice::copy_from_egl(node, pci) });
        }
        let mut devices = Vec::new();
        check(unsafe { dw_devices(collect, ptr::from_mut(&mut devices).cast()) })?;
        Ok(devices)
    }

    /// Create an offscreen runtime on the matching EGL device. Failure returns an error
    /// without falling back to automatic device selection.
    pub fn new_on_device(
        width: u32,
        height: u32,
        selector: DeviceSelector<'_>,
    ) -> Result<Self, Error> {
        let (by_pci, bytes) = match selector {
            DeviceSelector::RenderNode(path) => (0, path.as_os_str().as_bytes()),
            DeviceSelector::Pci(address) => (1, address.as_bytes()),
        };
        Self::create(width, height, |out| unsafe {
            dw_create_on_device(
                width as i32,
                height as i32,
                by_pci,
                bytes.as_ptr(),
                bytes.len(),
                out,
            )
        })
    }

    /// The active display's EGL device when reported by the driver. For a software
    /// device, the result may have no DRM render node or PCI address.
    pub fn active_device(&self) -> Option<RenderDevice> {
        extern "C" fn store(data: *mut c_void, node: *const c_char, pci: *const c_char) {
            // The live runtime visits at most once, synchronously, with this data pointer.
            *unsafe { &mut *data.cast::<Option<RenderDevice>>() } =
                Some(unsafe { RenderDevice::copy_from_egl(node, pci) });
        }
        let mut device = None;
        unsafe { dw_active_device(self.raw.as_ptr(), store, ptr::from_mut(&mut device).cast()) };
        device
    }

    /// Create an EGL window surface on the caller's Wayland connection.
    ///
    /// # Safety
    /// `display` must point to a live libwayland `wl_display`. `window` must point to a
    /// live `wl_egl_window` created on a configured `wl_surface` from that display,
    /// with the supplied pixel size. Keep them alive, at that size, until this runtime
    /// is dropped and use the surface and its EGL resources on this thread only.
    /// This runtime initializes and terminates the EGL display. Do not share it with
    /// other EGL users. Do not switch the current EGL context while the runtime lives.
    pub unsafe fn new_wayland_window(
        width: u32,
        height: u32,
        display: *mut c_void,
        window: *mut c_void,
    ) -> Result<Self, Error> {
        Self::create(width, height, |out| unsafe {
            dw_create_wayland(width as i32, height as i32, display, window, out)
        })
    }

    fn create(
        width: u32,
        height: u32,
        init: impl FnOnce(*mut *mut c_void) -> i32,
    ) -> Result<Self, Error> {
        let frame_len = super::rgba_len(width, height)
            .ok_or_else(|| Error("frame dimensions must be in 1..=8192".into()))?;
        let mut raw = ptr::null_mut();
        check(init(&mut raw))?;
        Ok(Self {
            raw: NonNull::new(raw).ok_or_else(|| Error("Cubism returned a null runtime".into()))?,
            frame_len,
            frame_size: (width, height),
            _thread: PhantomData,
        })
    }

    /// The size of the nearest-sampled target used for model input regions.
    pub fn window_mask_size(&self) -> (u32, u32) {
        (self.frame_size.0.div_ceil(8), self.frame_size.1.div_ceil(8))
    }

    /// Read the staged frame's input samples as top-down RGBA8. Use the alpha channel.
    pub fn read_window_mask(&self, rgba: &mut [u8]) -> Result<(), Error> {
        self.read_staged_window(true, rgba)
    }

    /// Read the actual staged window back buffer as top-down RGBA8 for diagnostics.
    pub fn read_window_pixels(&self, rgba: &mut [u8]) -> Result<(), Error> {
        self.read_staged_window(false, rgba)
    }

    fn read_staged_window(&self, sampled: bool, rgba: &mut [u8]) -> Result<(), Error> {
        let expected = if sampled {
            let (width, height) = self.window_mask_size();
            super::rgba_len(width, height).expect("bounded mask size")
        } else {
            self.frame_len
        };
        if rgba.len() != expected {
            return Err(Error("invalid RGBA buffer length".into()));
        }
        // The live runtime writes synchronously into the checked exclusive slice.
        check(unsafe {
            dw_read_window(
                self.raw.as_ptr(),
                i32::from(sampled),
                rgba.as_mut_ptr(),
                rgba.len(),
            )
        })
    }

    /// Swap the staged frame and commit pending Wayland state, including its input region.
    pub fn present_window(&self) -> Result<(), Error> {
        // The runtime borrows the native window for its full lifetime.
        check(unsafe { dw_present_window(self.raw.as_ptr()) })
    }

    pub fn renderer(&self) -> String {
        // The runtime owns this immutable NUL-terminated string for its full lifetime.
        unsafe { CStr::from_ptr(dw_renderer(self.raw.as_ptr())) }
            .to_string_lossy()
            .into_owned()
    }

    /// Load a model and its assets synchronously. The PoC is intended for trusted model packages.
    pub fn load(&self, model_json: &Path) -> Result<Model<'_>, Error> {
        let path = model_json.as_os_str().as_bytes();
        let mut raw = ptr::null_mut();
        // The shim reads the path only during this call. The model borrows its live runtime.
        check(unsafe { dw_load(self.raw.as_ptr(), path.as_ptr(), path.len(), &mut raw) })?;
        Ok(Model {
            raw: NonNull::new(raw).ok_or_else(|| Error("Cubism returned a null model".into()))?,
            runtime: self,
        })
    }
}

impl Model<'_> {
    pub fn uses_masking(&self) -> bool {
        // The shim inspects the live model without changing it.
        unsafe { dw_uses_masking(self.raw.as_ptr()) != 0 }
    }

    pub fn motion_groups(&self) -> Result<Vec<(String, u32)>, Error> {
        self.metadata(false)
    }

    pub fn expressions(&self) -> Result<Vec<String>, Error> {
        Ok(self
            .metadata(true)?
            .into_iter()
            .map(|(name, _)| name)
            .collect())
    }

    fn metadata(&self, expressions: bool) -> Result<Vec<(String, u32)>, Error> {
        let expressions = i32::from(expressions);
        // The handle remains live, and the shim reads model settings on this thread.
        let count = unsafe { dw_metadata_count(self.raw.as_ptr(), expressions) };
        if !(0..=128).contains(&count) {
            return Err(Error("unsupported model metadata count".into()));
        }
        (0..count)
            .map(|index| {
                let mut name = ptr::null();
                let mut motions = 0;
                // Copy the borrowed C string before the next shim call.
                check(unsafe {
                    dw_metadata_entry(
                        self.raw.as_ptr(),
                        expressions,
                        index,
                        &mut name,
                        &mut motions,
                    )
                })?;
                if name.is_null() || motions < 0 {
                    return Err(Error("invalid Cubism metadata".into()));
                }
                let name = unsafe { CStr::from_ptr(name) }
                    .to_str()
                    .map_err(|_| Error("model metadata name is not UTF-8".into()))?;
                if name.len() > 128 {
                    return Err(Error("model metadata name is too long".into()));
                }
                Ok((name.to_owned(), motions as u32))
            })
            .collect()
    }

    pub fn play_expression(&mut self, name: &str) -> Result<(), Error> {
        // The shim reads the name synchronously while the model is live.
        check(unsafe { dw_expression(self.raw.as_ptr(), name.as_ptr(), name.len()) })
    }

    /// Configure when ordinary expressions reset, measured in rendered seconds since
    /// the last expression started. `None` disables the timer (the default).
    /// This does not affect persistent toggles.
    pub fn set_expression_timeout(&mut self, timeout: Option<f32>) -> Result<(), Error> {
        let timeout = optional_seconds(timeout)?;
        // Changes only the timeout on the live model.
        check(unsafe { dw_set_expression_timeout(self.raw.as_ptr(), timeout) })
    }

    /// Flip the persistent layer for a model-relative expression asset. Returns its
    /// new target state. Fades and optional expiry advance with render time, and do
    /// not alter the ordinary expression queue. Zero fade applies on the next draw.
    pub fn toggle_expression(
        &mut self,
        file: &Path,
        fade_seconds: f32,
        deactivate_after: Option<f32>,
    ) -> Result<bool, Error> {
        let fade = optional_seconds(Some(fade_seconds))?;
        let duration = optional_seconds(deactivate_after)?;
        let file = file.as_os_str().as_bytes();
        let mut active = 0;
        // The shim synchronously copies the path and writes the target state.
        check(unsafe {
            dw_toggle_expression(
                self.raw.as_ptr(),
                file.as_ptr(),
                file.len(),
                fade,
                duration,
                &mut active,
            )
        })?;
        Ok(active != 0)
    }

    /// Query the target state, not the current fade weight. An unplayed valid
    /// relative path, an expired toggle, or a toggle fading out returns `false`.
    pub fn expression_toggle_active(&self, file: &Path) -> Result<bool, Error> {
        let file = file.as_os_str().as_bytes();
        let mut active = 0;
        // The shim reads only the current model and writes the state synchronously.
        check(unsafe {
            dw_expression_toggle_active(self.raw.as_ptr(), file.as_ptr(), file.len(), &mut active)
        })?;
        Ok(active != 0)
    }

    /// Immediately remove all persistent toggles and their timers, without
    /// stopping or restarting the ordinary expression.
    pub fn reset_expression_toggles(&mut self) {
        // The live model owns all toggle storage.
        unsafe { dw_reset_expression_toggles(self.raw.as_ptr()) }
    }

    pub fn play_motion(&mut self, group: &str, index: u32) -> Result<(), Error> {
        let index = i32::try_from(index).map_err(|_| Error("motion index out of range".into()))?;
        // The model is live and the group string is borrowed only for this synchronous call.
        check(unsafe { dw_motion(self.raw.as_ptr(), group.as_ptr(), group.len(), index) })
    }

    /// Cycle model motion groups using their first motion. With no motion groups,
    /// cycle expressions instead. With neither, this is a no-op.
    pub fn switch_skin(&mut self) -> Result<(), Error> {
        // The shim updates only this live model.
        check(unsafe { dw_switch_skin(self.raw.as_ptr()) })
    }

    /// Clear look and apply the legacy tap action using the latest drawable geometry.
    /// Coordinates are finite and in `0..=1`, measured from the top-left of the
    /// actual render buffer. The caller is responsible for clipping to its input region.
    /// Head cycles expressions. Body starts a random `TapBody` motion when its
    /// priority permits and also cycles expressions. Missing groups or expressions
    /// are a no-op. Models with no declared hit areas split at `y = 0.5`.
    pub fn tap(&mut self, x: f32, y: f32) -> Result<TapHit, Error> {
        if !(0.0..=1.0).contains(&x) || !(0.0..=1.0).contains(&y) {
            return Err(Error("tap coordinates must be finite and in 0..=1".into()));
        }
        let mut hit = 0;
        // The shim only accesses the live model and writes synchronously to `hit`.
        check(unsafe { dw_tap(self.raw.as_ptr(), x, y, &mut hit) })?;
        match hit {
            0 => Ok(TapHit::Miss),
            1 => Ok(TapHit::Head),
            2 => Ok(TapHit::Body),
            _ => Err(Error("invalid Cubism tap result".into())),
        }
    }

    /// Override the model's declared `LipSync` parameters on subsequent frames.
    /// `Some(0.0)` closes the mouth; `None` returns control to animation.
    /// Models without declared lip-sync parameters are unaffected.
    pub fn set_lip_sync(&mut self, opening: Option<f32>) -> Result<(), Error> {
        if opening.is_some_and(|value| !(0.0..=1.0).contains(&value)) {
            return Err(Error("lip sync must be finite and in 0..=1".into()));
        }
        // This only updates state on the live model; it does not access an output buffer.
        check(unsafe {
            dw_set_lip_sync(
                self.raw.as_ptr(),
                i32::from(opening.is_some()),
                opening.unwrap_or_default(),
            )
        })
    }

    /// Add a gaze offset to animation on subsequent frames, with positive x right
    /// and positive y up. Both axes must be finite and in `-1..=1`.
    /// `None` clears the offset. The caller can interpolate targets if needed.
    pub fn set_look(&mut self, target: Option<(f32, f32)>) -> Result<(), Error> {
        let (x, y) = target.unwrap_or_default();
        if !(-1.0..=1.0).contains(&x) || !(-1.0..=1.0).contains(&y) {
            return Err(Error("look axes must be finite and in -1..=1".into()));
        }
        // The shim stores the pair on this live model only, with no borrowed data.
        check(unsafe { dw_set_look(self.raw.as_ptr(), i32::from(target.is_some()), x, y) })
    }

    /// Update and draw to the window's back buffer without committing the surface.
    pub fn draw_window(&mut self, dt: f32) -> Result<(), Error> {
        validate_dt(dt)?;
        // The model borrows its runtime and both are live on the render thread.
        check(unsafe { dw_draw_window(self.raw.as_ptr(), dt) })
    }

    /// Update the model by `dt` seconds, then draw a tightly packed, top-down, premultiplied RGBA8 frame.
    pub fn render(&mut self, dt: f32, rgba: &mut [u8]) -> Result<(), Error> {
        validate_dt(dt)?;
        if rgba.len() != self.runtime.frame_len {
            return Err(Error("invalid RGBA buffer length".into()));
        }
        // The shim synchronously writes at most the validated length into this exclusive slice.
        check(unsafe { dw_frame(self.raw.as_ptr(), dt, rgba.as_mut_ptr(), rgba.len()) })
    }
}

fn optional_seconds(value: Option<f32>) -> Result<f32, Error> {
    match value {
        None => Ok(-1.0),
        Some(value) if value.is_finite() && value >= 0.0 => Ok(value),
        Some(_) => Err(Error(
            "expression duration must be finite and nonnegative".into(),
        )),
    }
}

fn validate_dt(dt: f32) -> Result<(), Error> {
    if !(0.0..=1.0).contains(&dt) {
        return Err(Error(
            "frame step must be finite and in 0..=1 seconds".into(),
        ));
    }
    Ok(())
}

impl Drop for Runtime {
    fn drop(&mut self) {
        // Each model borrows this runtime so safe Rust cannot drop it first.
        unsafe { dw_destroy(self.raw.as_ptr()) }
    }
}
impl Drop for Model<'_> {
    fn drop(&mut self) {
        // The native object belongs to this handle and its context remains alive.
        unsafe { dw_model_destroy(self.raw.as_ptr()) }
    }
}
