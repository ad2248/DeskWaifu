//! OCR inference engine: detection (DB) and recognition (CTC) through the
//! system ONNX Runtime.
//!
//! This crate owns preprocessing, DB postprocessing, box extraction, line
//! cropping, recognition resizing and CTC decoding. It has no Qt, clipboard,
//! plugin, IPC or model-downloading concerns; see `README.md` for scope.
//!
//! Geometry primitives that the C++ obtained from OpenCV (`resize`,
//! `warpPerspective`, `minAreaRect`, `findContours`, `fillPoly`) are ported in
//! `geometry` and `contours` and validated against OpenCV's own output.

pub mod contours;
pub mod detect;
pub mod engine;
pub mod geometry;
pub mod metadata;
pub mod onnx;
pub mod recognition;

pub use engine::{decode_image, Engine, Line, Recognition, MAX_IMAGE_BYTES, MAX_IMAGE_SIDE};
pub use geometry::Rgb;
pub use metadata::{DetectorMetadata, Normalization, PostProcess, RecognizerMetadata};
pub use onnx::OrtError;

/// An image-space point, matching the native floating-point box coordinates.
pub use deskwaifu_perception_domain::line_order::Point;

/// A text line's quadrilateral: top-left, top-right, bottom-right, bottom-left.
pub type LineBox = [Point; 4];

/// Failures raised while loading a model or recognising an image.
///
/// `code` mirrors the stable error codes the C++ engine reports so callers can
/// keep their protocol unchanged.
#[derive(Debug)]
pub enum Error {
    /// A model file or its metadata is missing (`model_missing`).
    ModelMissing(String),
    /// Metadata or ONNX shapes are unsupported (`model_incompatible`).
    ModelIncompatible(String),
    /// ONNX Runtime could not load the model (`model_load_failed`).
    ModelLoadFailed(String),
    /// Inference failed after a model was accepted.
    Inference(String),
}

impl Error {
    /// Stable protocol error code for this failure.
    pub fn code(&self) -> &'static str {
        match self {
            Error::ModelMissing(_) => "model_missing",
            Error::ModelIncompatible(_) => "model_incompatible",
            Error::ModelLoadFailed(_) => "model_load_failed",
            Error::Inference(_) => "backend_error",
        }
    }
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::ModelMissing(message)
            | Error::ModelIncompatible(message)
            | Error::ModelLoadFailed(message)
            | Error::Inference(message) => f.write_str(message),
        }
    }
}

impl std::error::Error for Error {}
