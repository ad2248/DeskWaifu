//! Hand-written binding to the ONNX Runtime C API.
//!
//! The system library (`libonnxruntime.so`, never vendored) is reached through
//! `OrtGetApiBase`. `OrtApi` is an append-only table of function pointers, so a
//! prefix of it is declared here. Every slot lists the index it occupies; the
//! indices were verified against the compiler-computed `offsetof` layout of the
//! ONNX Runtime 1.16 and 1.29 headers. Slots this crate never calls are still
//! declared, as pointer-sized placeholders, so the later offsets line up.

use std::ffi::{CStr, CString};
use std::fmt;
use std::os::raw::{c_char, c_void};
use std::path::Path;
use std::ptr;

/// Oldest API snapshot accepted. Every slot used below has been stable since
/// 1.12, so requesting 1.16 keeps older system runtimes usable.
const ORT_API_VERSION: u32 = 16;

const ORT_LOGGING_LEVEL_ERROR: i32 = 3; // OrtLoggingLevel::ORT_LOGGING_LEVEL_ERROR
const ORT_ENABLE_ALL: i32 = 99; // GraphOptimizationLevel::ORT_ENABLE_ALL
const ONNX_TYPE_TENSOR: i32 = 1; // ONNXType::ONNX_TYPE_TENSOR
/// `ONNXTensorElementDataType::ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT`.
const ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT: i32 = 1;
const ORT_ARENA_ALLOCATOR: i32 = 1; // OrtAllocatorType::OrtArenaAllocator
const ORT_MEM_TYPE_DEFAULT: i32 = 0; // OrtMemType::OrtMemTypeDefault

macro_rules! opaque {
    ($($name:ident),* $(,)?) => {
        $(
            #[repr(C)]
            pub struct $name {
                _private: [u8; 0],
            }
        )*
    };
}

opaque!(
    Status,
    Env,
    SessionHandle,
    SessionOptions,
    Value,
    MemoryInfo,
    Allocator,
    TypeInfo,
    TensorTypeAndShapeInfo,
);

#[repr(C)]
struct OrtApiBase {
    get_api: unsafe extern "C" fn(u32) -> *const OrtApi,
    get_version_string: unsafe extern "C" fn() -> *const c_char,
}

extern "C" {
    fn OrtGetApiBase() -> *const OrtApiBase;
}

#[repr(C)]
struct OrtApi {
    create_status: unsafe extern "C" fn(i32, *const c_char) -> *mut Status, // 0
    get_error_code: unsafe extern "C" fn(*const Status) -> i32,             // 1
    get_error_message: unsafe extern "C" fn(*const Status) -> *const c_char, // 2
    create_env: unsafe extern "C" fn(i32, *const c_char, *mut *mut Env) -> *mut Status, // 3
    _reserved_4_6: [*const (); 3], // 4 CreateEnvWithCustomLogger, 5 EnableTelemetryEvents, 6 DisableTelemetryEvents
    create_session: unsafe extern "C" fn(
        *const Env,
        *const c_char,
        *const SessionOptions,
        *mut *mut SessionHandle,
    ) -> *mut Status, // 7
    _reserved_8: [*const (); 1],   // 8 CreateSessionFromArray
    run: unsafe extern "C" fn(
        *mut SessionHandle,
        *const c_void,
        *const *const c_char,
        *const *const Value,
        usize,
        *const *const c_char,
        usize,
        *mut *mut Value,
    ) -> *mut Status, // 9
    create_session_options: unsafe extern "C" fn(*mut *mut SessionOptions) -> *mut Status, // 10
    _reserved_11_22: [*const (); 12], // 11..=22
    set_session_graph_optimization_level:
        unsafe extern "C" fn(*mut SessionOptions, i32) -> *mut Status, // 23
    set_intra_op_num_threads: unsafe extern "C" fn(*mut SessionOptions, i32) -> *mut Status, // 24
    set_inter_op_num_threads: unsafe extern "C" fn(*mut SessionOptions, i32) -> *mut Status, // 25
    _reserved_26_29: [*const (); 4], // 26..=29
    session_get_input_count: unsafe extern "C" fn(*const SessionHandle, *mut usize) -> *mut Status, // 30
    session_get_output_count: unsafe extern "C" fn(*const SessionHandle, *mut usize) -> *mut Status, // 31
    _reserved_32: [*const (); 1], // 32 SessionGetOverridableInitializerCount
    session_get_input_type_info:
        unsafe extern "C" fn(*const SessionHandle, usize, *mut *mut TypeInfo) -> *mut Status, // 33
    session_get_output_type_info:
        unsafe extern "C" fn(*const SessionHandle, usize, *mut *mut TypeInfo) -> *mut Status, // 34
    _reserved_35: [*const (); 1], // 35 SessionGetOverridableInitializerTypeInfo
    session_get_input_name: unsafe extern "C" fn(
        *const SessionHandle,
        usize,
        *mut Allocator,
        *mut *mut c_char,
    ) -> *mut Status, // 36
    session_get_output_name: unsafe extern "C" fn(
        *const SessionHandle,
        usize,
        *mut Allocator,
        *mut *mut c_char,
    ) -> *mut Status, // 37
    _reserved_38_48: [*const (); 11], // 38..=48
    create_tensor_with_data_as_ort_value: unsafe extern "C" fn(
        *const MemoryInfo,
        *mut c_void,
        usize,
        *const i64,
        usize,
        i32,
        *mut *mut Value,
    ) -> *mut Status, // 49
    is_tensor: unsafe extern "C" fn(*const Value, *mut i32) -> *mut Status, // 50
    get_tensor_mutable_data: unsafe extern "C" fn(*mut Value, *mut *mut c_void) -> *mut Status, // 51
    _reserved_52_54: [*const (); 3], // 52..=54
    cast_type_info_to_tensor_info:
        unsafe extern "C" fn(*const TypeInfo, *mut *const TensorTypeAndShapeInfo) -> *mut Status, // 55
    get_onnx_type_from_type_info: unsafe extern "C" fn(*const TypeInfo, *mut i32) -> *mut Status, // 56
    _reserved_57_59: [*const (); 3], // 57..=59
    get_tensor_element_type:
        unsafe extern "C" fn(*const TensorTypeAndShapeInfo, *mut i32) -> *mut Status, // 60
    get_dimensions_count:
        unsafe extern "C" fn(*const TensorTypeAndShapeInfo, *mut usize) -> *mut Status, // 61
    get_dimensions:
        unsafe extern "C" fn(*const TensorTypeAndShapeInfo, *mut i64, usize) -> *mut Status, // 62
    _reserved_63: [*const (); 1],    // 63 GetSymbolicDimensions
    get_tensor_shape_element_count:
        unsafe extern "C" fn(*const TensorTypeAndShapeInfo, *mut usize) -> *mut Status, // 64
    get_tensor_type_and_shape:
        unsafe extern "C" fn(*mut Value, *mut *mut TensorTypeAndShapeInfo) -> *mut Status, // 65
    _reserved_66_68: [*const (); 3], // 66..=68
    create_cpu_memory_info: unsafe extern "C" fn(i32, i32, *mut *mut MemoryInfo) -> *mut Status, // 69
    _reserved_70_75: [*const (); 6], // 70..=75
    allocator_free: unsafe extern "C" fn(*mut Allocator, *mut c_void), // 76
    _reserved_77: [*const (); 1],    // 77 AllocatorGetInfo
    get_allocator_with_default_options: unsafe extern "C" fn(*mut *mut Allocator) -> *mut Status, // 78
    _reserved_79_91: [*const (); 13],                  // 79..=91
    release_env: unsafe extern "C" fn(*mut Env),       // 92
    release_status: unsafe extern "C" fn(*mut Status), // 93
    release_memory_info: unsafe extern "C" fn(*mut MemoryInfo), // 94
    release_session: unsafe extern "C" fn(*mut SessionHandle), // 95
    release_value: unsafe extern "C" fn(*mut Value),   // 96
    _reserved_97: [*const (); 1],                      // 97 ReleaseRunOptions
    release_type_info: unsafe extern "C" fn(*mut TypeInfo), // 98
    release_tensor_type_and_shape_info: unsafe extern "C" fn(*mut TensorTypeAndShapeInfo), // 99
    release_session_options: unsafe extern "C" fn(*mut SessionOptions), // 100
}

/// Errors raised by the ONNX Runtime binding.
#[derive(Debug)]
pub enum OrtError {
    /// The shared library or the requested C API version was not available.
    Unavailable(String),
    /// An ONNX Runtime call returned a failure status.
    Status(String),
}

impl fmt::Display for OrtError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            OrtError::Unavailable(message) | OrtError::Status(message) => f.write_str(message),
        }
    }
}

impl std::error::Error for OrtError {}

/// The process-wide `OrtApi` table.
fn api() -> Result<&'static OrtApi, OrtError> {
    // SAFETY: `OrtGetApiBase` returns a pointer to a static `OrtApiBase`.
    let base = unsafe { OrtGetApiBase() };
    if base.is_null() {
        return Err(OrtError::Unavailable(
            "onnxruntime: OrtGetApiBase returned null".into(),
        ));
    }
    // SAFETY: `base` points at a live `OrtApiBase`.
    let table = unsafe { ((*base).get_api)(ORT_API_VERSION) };
    if table.is_null() {
        // SAFETY: the version string is a static buffer owned by the runtime.
        let version = unsafe {
            let text = ((*base).get_version_string)();
            if text.is_null() {
                "unknown".to_string()
            } else {
                CStr::from_ptr(text).to_string_lossy().into_owned()
            }
        };
        return Err(OrtError::Unavailable(format!(
            "onnxruntime {version} does not provide C API version {ORT_API_VERSION}"
        )));
    }
    // SAFETY: the table lives as long as the loaded shared library.
    Ok(unsafe { &*table })
}

/// Convert a nullable status into a `Result`, releasing it.
fn check(api: &OrtApi, status: *mut Status) -> Result<(), OrtError> {
    if status.is_null() {
        return Ok(());
    }
    // SAFETY: `status` is a live OrtStatus; the message is owned by it and is
    // copied before the status is released.
    let message = unsafe {
        let text = (api.get_error_message)(status);
        if text.is_null() {
            "unknown ONNX Runtime error".to_string()
        } else {
            CStr::from_ptr(text).to_string_lossy().into_owned()
        }
    };
    // SAFETY: `status` was produced by an ORT call and is released exactly once.
    unsafe { (api.release_status)(status) };
    Err(OrtError::Status(message))
}

/// Session configuration: two intra-op threads, one inter-op thread and full
/// graph optimization, matching `OcrEngine.cpp`.
pub struct Options {
    raw: *mut SessionOptions,
}

impl Options {
    pub fn new() -> Result<Self, OrtError> {
        let api = api()?;
        let mut raw = ptr::null_mut();
        // SAFETY: `raw` is a valid out-pointer.
        check(api, unsafe { (api.create_session_options)(&mut raw) })?;
        let result = (|| {
            // SAFETY: `raw` is a live OrtSessionOptions.
            check(api, unsafe { (api.set_intra_op_num_threads)(raw, 2) })?;
            check(api, unsafe { (api.set_inter_op_num_threads)(raw, 1) })?;
            check(api, unsafe {
                (api.set_session_graph_optimization_level)(raw, ORT_ENABLE_ALL)
            })
        })();
        match result {
            Ok(()) => Ok(Options { raw }),
            Err(error) => {
                // SAFETY: `raw` was created above and is released once.
                unsafe { (api.release_session_options)(raw) };
                Err(error)
            }
        }
    }
}

impl Drop for Options {
    fn drop(&mut self) {
        if let Ok(api) = api() {
            // SAFETY: `raw` came from CreateSessionOptions and is released once.
            unsafe { (api.release_session_options)(self.raw) };
        }
    }
}

/// Declared model signature, with `-1` for dynamic dimensions.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Signature {
    pub input_count: usize,
    pub output_count: usize,
    pub input_is_tensor: bool,
    pub output_is_tensor: bool,
    pub input_is_float: bool,
    pub output_is_float: bool,
    pub input_shape: Vec<i64>,
    pub output_shape: Vec<i64>,
}

/// A float32 model output.
pub struct Output {
    pub shape: Vec<i64>,
    pub data: Vec<f32>,
}

/// A loaded ONNX model plus its single input and output names.
pub struct Session {
    raw: *mut SessionHandle,
    api: &'static OrtApi,
    env: *mut Env,
    memory: *mut MemoryInfo,
    input: CString,
    output: CString,
}

struct TensorInfo<'a> {
    raw: *mut TensorTypeAndShapeInfo,
    api: &'a OrtApi,
}

impl Drop for TensorInfo<'_> {
    fn drop(&mut self) {
        // SAFETY: `raw` is a live tensor type/shape info released exactly once.
        unsafe { (self.api.release_tensor_type_and_shape_info)(self.raw) };
    }
}

struct TypeInfoGuard<'a> {
    raw: *mut TypeInfo,
    api: &'a OrtApi,
}

impl Drop for TypeInfoGuard<'_> {
    fn drop(&mut self) {
        if !self.raw.is_null() {
            // SAFETY: `raw` came from SessionGet*TypeInfo and is released once.
            unsafe { (self.api.release_type_info)(self.raw) };
        }
    }
}

struct ValueGuard<'a> {
    raw: *mut Value,
    api: &'a OrtApi,
}

impl Drop for ValueGuard<'_> {
    fn drop(&mut self) {
        if !self.raw.is_null() {
            // SAFETY: `raw` is a live OrtValue owned by this guard.
            unsafe { (self.api.release_value)(self.raw) };
        }
    }
}

/// Read the element type and shape of a live type info.
fn type_shape(api: &OrtApi, info: *mut TypeInfo) -> Result<(bool, bool, Vec<i64>), OrtError> {
    // SAFETY: `info` is a live OrtTypeInfo.
    let mut kind = 0i32;
    check(api, unsafe {
        (api.get_onnx_type_from_type_info)(info, &mut kind)
    })?;
    let mut tensor: *const TensorTypeAndShapeInfo = ptr::null();
    // SAFETY: `tensor` is a valid out-pointer; the C API writes null for
    // non-tensor types.
    check(api, unsafe {
        (api.cast_type_info_to_tensor_info)(info, &mut tensor)
    })?;
    if tensor.is_null() {
        return Ok((kind == ONNX_TYPE_TENSOR, false, Vec::new()));
    }
    // `CastTypeInfoToTensorInfo` returns a *borrowed* pointer: the C API says
    // "Do not free this value, it will be valid until type_info is freed."
    // The caller holds the owning `TypeInfoGuard`, so no release happens here.
    // SAFETY: `tensor` is valid while the owning type info lives.
    let mut element = 0i32;
    check(api, unsafe {
        (api.get_tensor_element_type)(tensor, &mut element)
    })?;
    let mut count = 0usize;
    check(api, unsafe {
        (api.get_dimensions_count)(tensor, &mut count)
    })?;
    let mut shape = vec![0i64; count];
    if count > 0 {
        check(api, unsafe {
            (api.get_dimensions)(tensor, shape.as_mut_ptr(), count)
        })?;
    }
    Ok((
        kind == ONNX_TYPE_TENSOR,
        element == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
        shape,
    ))
}

/// The process-wide default allocator; never freed by callers.
fn default_allocator() -> Result<*mut Allocator, OrtError> {
    let api = api()?;
    let mut allocator = ptr::null_mut();
    // SAFETY: `allocator` is a valid out-pointer.
    check(api, unsafe {
        (api.get_allocator_with_default_options)(&mut allocator)
    })?;
    Ok(allocator)
}

impl Session {
    /// Load `inference.onnx` from `directory` with the supplied options. The
    /// caller performs the C++ `model_missing` checks before calling.
    pub fn load(directory: &Path, options: &Options) -> Result<Self, OrtError> {
        let api = api()?;
        let mut env = ptr::null_mut();
        // SAFETY: the log id is a static NUL-terminated string.
        check(api, unsafe {
            (api.create_env)(ORT_LOGGING_LEVEL_ERROR, c"deskwaifu-ocr".as_ptr(), &mut env)
        })?;

        let path = directory.join("inference.onnx");
        let model = match path.to_str().and_then(|text| CString::new(text).ok()) {
            Some(model) => model,
            None => {
                // SAFETY: `env` was created above and is released once.
                unsafe { (api.release_env)(env) };
                return Err(OrtError::Status(
                    "ONNX model path is not valid UTF-8 without NUL bytes".into(),
                ));
            }
        };

        let mut raw = ptr::null_mut();
        // SAFETY: `env` is live and `model` is NUL-terminated.
        let status = unsafe { (api.create_session)(env, model.as_ptr(), options.raw, &mut raw) };
        if let Err(error) = check(api, status) {
            // SAFETY: `env` was created above and is released once.
            unsafe { (api.release_env)(env) };
            return Err(error);
        }
        let mut session = Session {
            raw,
            api,
            env,
            memory: ptr::null_mut(),
            input: CString::default(),
            output: CString::default(),
        };

        let mut memory = ptr::null_mut();
        // SAFETY: `memory` is a valid out-pointer.
        check(api, unsafe {
            (api.create_cpu_memory_info)(ORT_ARENA_ALLOCATOR, ORT_MEM_TYPE_DEFAULT, &mut memory)
        })?;
        session.memory = memory;

        let allocator = default_allocator()?;
        session.input = io_name(api, raw, allocator, true)?;
        session.output = io_name(api, raw, allocator, false)?;
        Ok(session)
    }

    /// Declared input/output types and shapes.
    pub fn signature(&self) -> Result<Signature, OrtError> {
        let api = self.api;
        let mut input_count = 0usize;
        // SAFETY: `self.raw` is a live session.
        check(api, unsafe {
            (api.session_get_input_count)(self.raw, &mut input_count)
        })?;
        let mut output_count = 0usize;
        check(api, unsafe {
            (api.session_get_output_count)(self.raw, &mut output_count)
        })?;
        let mut input = ptr::null_mut();
        let mut output = ptr::null_mut();
        if input_count > 0 {
            check(api, unsafe {
                (api.session_get_input_type_info)(self.raw, 0, &mut input)
            })?;
        }
        if output_count > 0 {
            check(api, unsafe {
                (api.session_get_output_type_info)(self.raw, 0, &mut output)
            })?;
        }
        let input_guard = TypeInfoGuard { raw: input, api };
        let output_guard = TypeInfoGuard { raw: output, api };
        let (input_is_tensor, input_is_float, input_shape) = if input.is_null() {
            (false, false, Vec::new())
        } else {
            type_shape(api, input)?
        };
        let (output_is_tensor, output_is_float, output_shape) = if output.is_null() {
            (false, false, Vec::new())
        } else {
            type_shape(api, output)?
        };
        drop(input_guard);
        drop(output_guard);
        Ok(Signature {
            input_count,
            output_count,
            input_is_tensor,
            output_is_tensor,
            input_is_float,
            output_is_float,
            input_shape,
            output_shape,
        })
    }

    /// Run one inference. `pixels` is `[1, 3, height, width]` in row-major order.
    pub fn run(&self, pixels: &[f32], height: i64, width: i64) -> Result<Output, OrtError> {
        let api = self.api;
        let shape = [1i64, 3, height, width];
        let expected = 3usize
            .checked_mul(height.max(0) as usize)
            .and_then(|value| value.checked_mul(width.max(0) as usize))
            .ok_or_else(|| OrtError::Status("ONNX input shape overflows".into()))?;
        if pixels.len() != expected {
            return Err(OrtError::Status(format!(
                "ONNX input has {} floats, expected {expected}",
                pixels.len()
            )));
        }
        let mut value: *mut Value = ptr::null_mut();
        // SAFETY: `pixels` outlives the call, `shape` is a four-element array
        // and `value` is a valid out-pointer.
        check(api, unsafe {
            (api.create_tensor_with_data_as_ort_value)(
                self.memory,
                pixels.as_ptr().cast_mut().cast(),
                std::mem::size_of_val(pixels),
                shape.as_ptr(),
                shape.len(),
                ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                &mut value,
            )
        })?;
        let input = ValueGuard { raw: value, api };

        let input_names = [self.input.as_ptr()];
        let output_names = [self.output.as_ptr()];
        let inputs = [input.raw as *const Value];
        let mut outputs: [*mut Value; 1] = [ptr::null_mut()];
        // SAFETY: names and the input array are valid for the call, and
        // `outputs` is a one-element out array matching `output_names_len`.
        check(api, unsafe {
            (api.run)(
                self.raw,
                ptr::null(),
                input_names.as_ptr(),
                inputs.as_ptr(),
                1,
                output_names.as_ptr(),
                1,
                outputs.as_mut_ptr(),
            )
        })?;
        let raw_output = outputs[0];
        if raw_output.is_null() {
            return Err(OrtError::Status("ONNX Runtime returned no output".into()));
        }
        let output = ValueGuard {
            raw: raw_output,
            api,
        };
        // SAFETY: `output.raw` is a live OrtValue.
        let mut is_tensor = 0i32;
        check(api, unsafe { (api.is_tensor)(output.raw, &mut is_tensor) })?;
        if is_tensor == 0 {
            return Err(OrtError::Status("ONNX output is not a tensor".into()));
        }
        let mut info: *mut TensorTypeAndShapeInfo = ptr::null_mut();
        // SAFETY: `output.raw` is a live tensor; `info` is an out-pointer.
        check(api, unsafe {
            (api.get_tensor_type_and_shape)(output.raw, &mut info)
        })?;
        if info.is_null() {
            return Err(OrtError::Status("ONNX output has no shape".into()));
        }
        let info = TensorInfo { raw: info, api };
        let mut element = 0i32;
        check(api, unsafe {
            (api.get_tensor_element_type)(info.raw, &mut element)
        })?;
        if element != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT {
            return Err(OrtError::Status(
                "ONNX output is not a float32 tensor".into(),
            ));
        }
        let mut count = 0usize;
        check(api, unsafe {
            (api.get_dimensions_count)(info.raw, &mut count)
        })?;
        let mut shape = vec![0i64; count];
        if count > 0 {
            check(api, unsafe {
                (api.get_dimensions)(info.raw, shape.as_mut_ptr(), count)
            })?;
        }
        let mut elements = 0usize;
        check(api, unsafe {
            (api.get_tensor_shape_element_count)(info.raw, &mut elements)
        })?;
        let mut data: *mut c_void = ptr::null_mut();
        check(api, unsafe {
            (api.get_tensor_mutable_data)(output.raw, &mut data)
        })?;
        if data.is_null() && elements > 0 {
            return Err(OrtError::Status("ONNX output data is null".into()));
        }
        // SAFETY: the tensor holds `elements` float32 values.
        let data = unsafe { std::slice::from_raw_parts(data.cast::<f32>(), elements) }.to_vec();
        Ok(Output { shape, data })
    }
}

/// Copy a session input or output name into a `CString` and free the original.
fn io_name(
    api: &OrtApi,
    session: *mut SessionHandle,
    allocator: *mut Allocator,
    input: bool,
) -> Result<CString, OrtError> {
    let mut text: *mut c_char = ptr::null_mut();
    // SAFETY: `session` is live, `allocator` is the default allocator, and
    // `text` is a valid out-pointer.
    let status = unsafe {
        if input {
            (api.session_get_input_name)(session, 0, allocator, &mut text)
        } else {
            (api.session_get_output_name)(session, 0, allocator, &mut text)
        }
    };
    check(api, status)?;
    if text.is_null() {
        return Err(OrtError::Status("ONNX tensor name is null".into()));
    }
    // SAFETY: the runtime allocated `text`; it is copied then released with
    // the matching allocator free call.
    let name = unsafe { CStr::from_ptr(text) }.to_owned();
    // SAFETY: `text` came from the allocator above and is freed exactly once.
    unsafe { (api.allocator_free)(allocator, text.cast()) };
    Ok(name)
}

impl Drop for Session {
    fn drop(&mut self) {
        // SAFETY: all handles were created by this session and released once.
        unsafe {
            if !self.memory.is_null() {
                (self.api.release_memory_info)(self.memory);
            }
            (self.api.release_session)(self.raw);
            (self.api.release_env)(self.env);
        }
    }
}
