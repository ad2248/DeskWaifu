//! Thread-affine, owning bindings to the externally installed Cubism Core.
//! No SDK code or model assets are distributed with this crate.
#![deny(unsafe_op_in_unsafe_fn)]

mod ffi;

use std::{
    alloc::{alloc_zeroed, dealloc, Layout},
    ffi::{c_void, CStr},
    fmt,
    marker::PhantomData,
    ptr::NonNull,
    rc::Rc,
};

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    InvalidMoc,
    BufferTooLarge,
    AllocationFailed,
    ModelInitializationFailed,
    InvalidMetadata,
    ParameterIndexOutOfBounds,
    ParameterValueOutOfBounds,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::InvalidMoc => "invalid, inconsistent, or unsupported moc3 data",
            Self::BufferTooLarge => "buffer exceeds Core's 32-bit size limit",
            Self::AllocationFailed => "aligned allocation failed",
            Self::ModelInitializationFailed => "Core could not initialize the model",
            Self::InvalidMetadata => "Core returned invalid model metadata",
            Self::ParameterIndexOutOfBounds => "parameter index out of bounds",
            Self::ParameterValueOutOfBounds => {
                "parameter value must be finite and within its minimum/maximum"
            }
        })
    }
}
impl std::error::Error for Error {}

/// Core's packed version identifier, as returned by csmGetVersion.
pub fn version() -> u32 {
    // SAFETY: This Core query takes no pointers and has no preconditions.
    unsafe { ffi::csmGetVersion() }
}

pub fn latest_moc_version() -> u32 {
    // SAFETY: This Core query takes no pointers and has no preconditions.
    unsafe { ffi::csmGetLatestMocVersion() }
}

struct AlignedBuffer {
    ptr: NonNull<u8>,
    layout: Layout,
}

impl AlignedBuffer {
    fn new(size: usize, alignment: usize) -> Result<Self, Error> {
        if size == 0 {
            return Err(Error::AllocationFailed);
        }
        let layout = Layout::from_size_align(size, alignment).map_err(|_| Error::BufferTooLarge)?;
        // SAFETY: The layout has nonzero size and valid alignment. Null is handled below.
        let ptr = NonNull::new(unsafe { alloc_zeroed(layout) }).ok_or(Error::AllocationFailed)?;
        Ok(Self { ptr, layout })
    }

    fn as_ptr(&self) -> *mut c_void {
        self.ptr.as_ptr().cast()
    }
}

impl Drop for AlignedBuffer {
    fn drop(&mut self) {
        // SAFETY: This is the original allocation and layout, uniquely owned until drop.
        unsafe { dealloc(self.ptr.as_ptr(), self.layout) };
    }
}

struct MocInner {
    ptr: NonNull<ffi::Moc>,
    _buffer: AlignedBuffer,
    // Core objects must remain on their caller's thread.
    _thread: PhantomData<Rc<()>>,
}

/// A validated and revived moc, retained by every model created from it.
/// This type is deliberately neither Send nor Sync.
pub struct Moc {
    inner: Rc<MocInner>,
    format_version: u32,
}

impl Moc {
    /// Copies bytes into 64-byte aligned storage, validates consistency, then revives in place.
    /// Invalid data is never passed directly to the revival/initialization APIs.
    pub fn from_bytes(bytes: &[u8]) -> Result<Self, Error> {
        let size = u32::try_from(bytes.len()).map_err(|_| Error::BufferTooLarge)?;
        // The moc3 fixed header is 64 bytes. Reject short/bad magic before calling Core.
        if bytes.len() < 64 || &bytes[..4] != b"MOC3" {
            return Err(Error::InvalidMoc);
        }
        let buffer = AlignedBuffer::new(bytes.len(), 64)?;
        // SAFETY: The newly allocated buffer is disjoint from bytes and has exactly bytes.len() capacity.
        unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), buffer.ptr.as_ptr(), bytes.len()) };
        // SAFETY: Core's consistency checker accepts unrevived data with an explicit byte length;
        // the buffer is writable, fully initialized and aligned to csmAlignofMoc (64).
        if unsafe { ffi::csmHasMocConsistency(buffer.as_ptr(), size) } != 1 {
            return Err(Error::InvalidMoc);
        }
        // SAFETY: The buffer is a consistency-validated, unrevived moc of the given size.
        let format_version = unsafe { ffi::csmGetMocVersion(buffer.as_ptr(), size) };
        if format_version == 0 || format_version > latest_moc_version() {
            return Err(Error::InvalidMoc);
        }
        // SAFETY: Same validated, exclusively owned aligned buffer; revival happens exactly once.
        let ptr = NonNull::new(unsafe { ffi::csmReviveMocInPlace(buffer.as_ptr(), size) })
            .ok_or(Error::InvalidMoc)?;
        Ok(Self {
            inner: Rc::new(MocInner {
                ptr,
                _buffer: buffer,
                _thread: PhantomData,
            }),
            format_version,
        })
    }

    pub fn format_version(&self) -> u32 {
        self.format_version
    }

    pub fn create_model(&self) -> Result<Model, Error> {
        // SAFETY: The revived moc remains allocated through self.inner.
        let size = unsafe { ffi::csmGetSizeofModel(self.inner.ptr.as_ptr()) };
        if size == 0 {
            return Err(Error::ModelInitializationFailed);
        }
        let buffer = AlignedBuffer::new(size as usize, 16)?;
        // SAFETY: Moc is alive, and the exclusive zeroed model buffer has the requested size
        // and csmAlignofModel (16) alignment. Core retains pointers into the moc, kept below.
        let ptr = NonNull::new(unsafe {
            ffi::csmInitializeModelInPlace(self.inner.ptr.as_ptr(), buffer.as_ptr(), size)
        })
        .ok_or(Error::ModelInitializationFailed)?;
        let mut model = Model {
            ptr,
            _buffer: buffer,
            _moc: Rc::clone(&self.inner),
            canvas: CanvasInfo::default(),
            parameters: Vec::new(),
            _thread: PhantomData,
        };
        model.read_metadata()?;
        Ok(model)
    }
}

#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct CanvasInfo {
    pub size_in_pixels: [f32; 2],
    pub origin_in_pixels: [f32; 2],
    pub pixels_per_unit: f32,
}

/// Owned metadata, independent of the native model's storage.
#[derive(Debug, Clone, PartialEq)]
pub struct ParameterInfo {
    pub id: String,
    /// Core parameter type: 0 = normal, 1 = blend shape.
    pub parameter_type: i32,
    pub minimum: f32,
    pub maximum: f32,
    pub default: f32,
}

/// An initialized model with retained moc storage; neither Send nor Sync.
/// No native pointers or writable parameter slices escape this wrapper.
pub struct Model {
    ptr: NonNull<ffi::Model>,
    _buffer: AlignedBuffer,
    _moc: Rc<MocInner>,
    canvas: CanvasInfo,
    parameters: Vec<ParameterInfo>,
    _thread: PhantomData<Rc<()>>,
}

impl Model {
    fn read_metadata(&mut self) -> Result<(), Error> {
        let mut size = ffi::Vector2::default();
        let mut origin = ffi::Vector2::default();
        let mut pixels_per_unit = 0.0;
        // SAFETY: Model is initialized and live; all output pointers refer to distinct writable values.
        unsafe {
            ffi::csmReadCanvasInfo(
                self.ptr.as_ptr(),
                &mut size,
                &mut origin,
                &mut pixels_per_unit,
            )
        };
        self.canvas = CanvasInfo {
            size_in_pixels: [size.x, size.y],
            origin_in_pixels: [origin.x, origin.y],
            pixels_per_unit,
        };
        // SAFETY: The model is initialized and remains alive throughout these queries.
        let count = unsafe { ffi::csmGetParameterCount(self.ptr.as_ptr()) };
        let count = usize::try_from(count).map_err(|_| Error::InvalidMetadata)?;
        if count == 0 {
            return Ok(());
        }
        // SAFETY: Core returns count-sized arrays owned by this initialized model. They are
        // only read here and immediately copied into owned Rust metadata; nulls are checked.
        unsafe {
            let ids = ffi::csmGetParameterIds(self.ptr.as_ptr());
            let types = ffi::csmGetParameterTypes(self.ptr.as_ptr());
            let minimum = ffi::csmGetParameterMinimumValues(self.ptr.as_ptr());
            let maximum = ffi::csmGetParameterMaximumValues(self.ptr.as_ptr());
            let defaults = ffi::csmGetParameterDefaultValues(self.ptr.as_ptr());
            if ids.is_null()
                || types.is_null()
                || minimum.is_null()
                || maximum.is_null()
                || defaults.is_null()
            {
                return Err(Error::InvalidMetadata);
            }
            for index in 0..count {
                let id = *ids.add(index);
                if id.is_null() {
                    return Err(Error::InvalidMetadata);
                }
                self.parameters.push(ParameterInfo {
                    // Core documents each ID as a null-terminated ANSI string.
                    id: CStr::from_ptr(id).to_string_lossy().into_owned(),
                    parameter_type: *types.add(index),
                    minimum: *minimum.add(index),
                    maximum: *maximum.add(index),
                    default: *defaults.add(index),
                });
            }
        }
        Ok(())
    }

    pub fn canvas(&self) -> CanvasInfo {
        self.canvas
    }

    pub fn parameters(&self) -> &[ParameterInfo] {
        &self.parameters
    }

    pub fn parameter_index(&self, id: &str) -> Option<usize> {
        self.parameters
            .iter()
            .position(|parameter| parameter.id == id)
    }

    pub fn parameter_value(&self, index: usize) -> Result<f32, Error> {
        self.parameters
            .get(index)
            .ok_or(Error::ParameterIndexOutOfBounds)?;
        // SAFETY: The initialized model is live and index is checked against its parameter count.
        // Core exposes a mutable pointer, but this method only reads and never retains it.
        unsafe {
            let values = ffi::csmGetParameterValues(self.ptr.as_ptr());
            if values.is_null() {
                return Err(Error::InvalidMetadata);
            }
            Ok(*values.add(index))
        }
    }

    /// Rejects non-finite or out-of-range values instead of silently clamping them.
    pub fn set_parameter_value(&mut self, index: usize, value: f32) -> Result<(), Error> {
        let parameter = self
            .parameters
            .get(index)
            .ok_or(Error::ParameterIndexOutOfBounds)?;
        if !value.is_finite() || value < parameter.minimum || value > parameter.maximum {
            return Err(Error::ParameterValueOutOfBounds);
        }
        // SAFETY: Exclusive model access; index was checked against Core's parameter count.
        // The values pointer is only used while the initialized model is live.
        unsafe {
            let values = ffi::csmGetParameterValues(self.ptr.as_ptr());
            if values.is_null() {
                return Err(Error::InvalidMetadata);
            }
            *values.add(index) = value;
        }
        Ok(())
    }

    pub fn update(&mut self) {
        // SAFETY: Model and source moc are alive and exclusively accessed on their owner thread.
        unsafe { ffi::csmUpdateModel(self.ptr.as_ptr()) };
    }
}
