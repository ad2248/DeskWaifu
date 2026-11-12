//! `inference.yml` parsing and validation.
//!
//! Every `Require(...)` in `OcrEngine.cpp` is mirrored here, so an unsupported
//! model is rejected while loading instead of failing during inference. Values
//! are read the way `yaml-cpp` reads them: a scalar's text is used for string
//! comparisons and booleans accept the YAML 1.1 spellings.

use std::path::Path;
use yaml_rust2::{Yaml, YamlLoader};

use crate::Error;

/// Maximum accepted metadata size, matching the C++ 4 MiB limit.
const MAX_METADATA_BYTES: u64 = 4 * 1024 * 1024;

/// Detector normalization validated while loading.
#[derive(Clone, Debug, PartialEq)]
pub struct Normalization {
    pub mean: [f32; 3],
    pub stddev: [f32; 3],
    pub scale: String,
    pub order: String,
}

/// DB postprocessing parameters read from the detector metadata.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct PostProcess {
    /// Probability threshold for the bitmap (`thresh`).
    pub thresh: f32,
    /// Mean-probability threshold for keeping a box (`box_thresh`).
    pub box_thresh: f32,
    /// DB unclip expansion ratio.
    pub unclip_ratio: f32,
    /// Maximum number of contours inspected (`max_candidates`).
    pub max_candidates: usize,
}

/// Validated detector metadata.
#[derive(Clone, Debug, PartialEq)]
pub struct DetectorMetadata {
    pub normalization: Normalization,
    pub postprocess: PostProcess,
}

/// Validated recognizer metadata.
#[derive(Clone, Debug, PartialEq)]
pub struct RecognizerMetadata {
    /// Recognition input `[3, height, width]`, fixed at `[3,48,320]`.
    pub height: usize,
    pub width: usize,
    /// Class labels: blank first, then the dictionary, then a literal space.
    pub characters: Vec<String>,
}

fn incompatible(message: impl Into<String>) -> Error {
    Error::ModelIncompatible(message.into())
}

/// Read and parse `inference.yml` from `directory`.
fn metadata(directory: &Path, model_name: &str) -> Result<Yaml, Error> {
    let path = directory.join("inference.yml");
    let metadata = std::fs::metadata(&path).map_err(|_| {
        Error::ModelMissing(format!("Cannot read OCR metadata: {}", path.display()))
    })?;
    if !metadata.is_file() {
        return Err(Error::ModelMissing(format!(
            "Cannot read OCR metadata: {}",
            path.display()
        )));
    }
    if metadata.len() > MAX_METADATA_BYTES {
        return Err(incompatible(format!(
            "OCR metadata exceeds 4 MiB: {}",
            path.display()
        )));
    }
    let bytes = std::fs::read(&path).map_err(|_| {
        Error::ModelMissing(format!("Cannot read OCR metadata: {}", path.display()))
    })?;
    // `QString::fromStdString` folds invalid UTF-8 into U+FFFD, which the
    // dictionary validation then rejects; a lossy decode keeps that behavior.
    let text = String::from_utf8_lossy(&bytes);
    let documents = YamlLoader::load_from_str(&text).map_err(|error| {
        incompatible(format!("Invalid OCR metadata {}: {error}", path.display()))
    })?;
    let document = documents
        .into_iter()
        .next()
        .ok_or_else(|| incompatible(format!("Invalid OCR metadata {}: empty", path.display())))?;
    let name = scalar_text(&document["Global"]["model_name"]);
    if name.as_deref() != Some(model_name) {
        return Err(incompatible(format!(
            "Expected {model_name} metadata: {}",
            path.display()
        )));
    }
    Ok(document)
}

/// A scalar's text, as `yaml-cpp`'s `as<std::string>()` would return it.
fn scalar_text(node: &Yaml) -> Option<String> {
    match node {
        Yaml::String(text) => Some(text.clone()),
        Yaml::Real(text) => Some(text.clone()),
        Yaml::Integer(value) => Some(value.to_string()),
        Yaml::Boolean(value) => Some(value.to_string()),
        _ => None,
    }
}

/// True when the node holds a key, mirroring yaml-cpp's definedness test.
fn is_defined(node: &Yaml) -> bool {
    !matches!(node, Yaml::BadValue)
}

/// The value of a named preprocessing transform, mirroring `Transform(...)`.
fn transform<'a>(metadata: &'a Yaml, name: &str) -> Result<&'a Yaml, Error> {
    if let Some(transforms) = metadata["PreProcess"]["transform_ops"].as_vec() {
        for entry in transforms {
            if let Some(node) = entry.as_hash() {
                for (key, value) in node {
                    if key.as_str() == Some(name) {
                        return Ok(value);
                    }
                }
            }
        }
    }
    Err(incompatible(format!(
        "Missing preprocessing transform: {name}"
    )))
}

/// `yaml-cpp`'s flexible boolean spellings (`convert<bool>::decode`).
fn as_bool(node: &Yaml) -> Option<bool> {
    let text = scalar_text(node)?.to_ascii_lowercase();
    match text.as_str() {
        "y" | "yes" | "true" | "on" => Some(true),
        "n" | "no" | "false" | "off" => Some(false),
        _ => None,
    }
}

/// Parse a scalar the way `yaml-cpp` converts to `float`, including the
/// explicit `.nan`/`.inf` spellings that must later fail the finite check.
fn as_f32(node: &Yaml) -> Option<f32> {
    let text = scalar_text(node)?;
    let trimmed = text.trim();
    match trimmed.to_ascii_lowercase().as_str() {
        ".nan" | "+.nan" | "-.nan" => return Some(f32::NAN),
        ".inf" | "+.inf" => return Some(f32::INFINITY),
        "-.inf" => return Some(f32::NEG_INFINITY),
        _ => {}
    }
    trimmed.parse::<f32>().ok()
}

/// Parse an integer the way `yaml-cpp` streams into `int`: a real value is
/// truncated toward zero.
fn as_i64(node: &Yaml) -> Option<i64> {
    let text = scalar_text(node)?;
    let trimmed = text.trim();
    if let Ok(value) = trimmed.parse::<i64>() {
        return Some(value);
    }
    let value = trimmed.parse::<f64>().ok()?;
    if value.is_finite() {
        Some(value.trunc() as i64)
    } else {
        None
    }
}

fn f32_list(node: &Yaml) -> Option<Vec<f32>> {
    node.as_vec()?
        .iter()
        .map(as_f32)
        .collect::<Option<Vec<f32>>>()
}

fn i64_list(node: &Yaml) -> Option<Vec<i64>> {
    node.as_vec()?
        .iter()
        .map(as_i64)
        .collect::<Option<Vec<i64>>>()
}

fn require(condition: bool, message: &str) -> Result<(), Error> {
    if condition {
        Ok(())
    } else {
        Err(incompatible(message))
    }
}

/// Validate the detector `inference.yml` in `directory`.
pub fn load_detector(directory: &Path) -> Result<DetectorMetadata, Error> {
    let document = metadata(directory, "PP-OCRv6_small_det")?;
    let decode = transform(&document, "DecodeImage")?;
    require(
        scalar_text(&decode["img_mode"]).as_deref() == Some("BGR")
            && as_bool(&decode["channel_first"]) == Some(false),
        "Only BGR HWC image decoding is supported",
    )?;

    let normalization = transform(&document, "NormalizeImage")?;
    let mean = f32_list(&normalization["mean"])
        .ok_or_else(|| incompatible("Invalid OCR preprocessing/dictionary metadata"))?;
    let stddev = f32_list(&normalization["std"])
        .ok_or_else(|| incompatible("Invalid OCR preprocessing/dictionary metadata"))?;
    let scale = scalar_text(&normalization["scale"])
        .ok_or_else(|| incompatible("Invalid OCR preprocessing/dictionary metadata"))?;
    let order = scalar_text(&normalization["order"])
        .ok_or_else(|| incompatible("Invalid OCR preprocessing/dictionary metadata"))?;
    require(
        mean == [0.485, 0.456, 0.406]
            && stddev == [0.229, 0.224, 0.225]
            && scale == "1./255."
            && order == "hwc",
        "Unsupported detector normalization",
    )?;

    let resize = transform(&document, "DetResizeForTest")?;
    require(
        matches!(resize, Yaml::Null),
        "Unsupported detector resize configuration",
    )?;
    transform(&document, "ToCHWImage")?;

    let post = &document["PostProcess"];
    require(
        scalar_text(&post["name"]).as_deref() == Some("DBPostProcess"),
        "Unsupported OCR postprocessing",
    )?;
    let thresh = as_f32(&post["thresh"])
        .ok_or_else(|| incompatible("Invalid OCR preprocessing/dictionary metadata"))?;
    let box_thresh = as_f32(&post["box_thresh"])
        .ok_or_else(|| incompatible("Invalid OCR preprocessing/dictionary metadata"))?;
    let unclip_ratio = as_f32(&post["unclip_ratio"])
        .ok_or_else(|| incompatible("Invalid OCR preprocessing/dictionary metadata"))?;
    let max_candidates = as_i64(&post["max_candidates"])
        .ok_or_else(|| incompatible("Invalid OCR preprocessing/dictionary metadata"))?;
    require(
        thresh.is_finite()
            && thresh > 0.0
            && thresh < 1.0
            && box_thresh.is_finite()
            && box_thresh > 0.0
            && box_thresh < 1.0
            && unclip_ratio.is_finite()
            && unclip_ratio > 0.0
            && unclip_ratio <= 10.0
            && max_candidates > 0
            && max_candidates <= 10_000,
        "Invalid DB postprocessing parameters",
    )?;

    Ok(DetectorMetadata {
        normalization: Normalization {
            mean: [mean[0], mean[1], mean[2]],
            stddev: [stddev[0], stddev[1], stddev[2]],
            scale,
            order,
        },
        postprocess: PostProcess {
            thresh,
            box_thresh,
            unclip_ratio,
            max_candidates: max_candidates as usize,
        },
    })
}

/// Validate the recognizer `inference.yml` in `directory`.
pub fn load_recognizer(directory: &Path) -> Result<RecognizerMetadata, Error> {
    let document = metadata(directory, "PP-OCRv6_small_rec")?;
    let decode = transform(&document, "DecodeImage")?;
    require(
        scalar_text(&decode["img_mode"]).as_deref() == Some("BGR")
            && as_bool(&decode["channel_first"]) == Some(false),
        "Only BGR HWC image decoding is supported",
    )?;

    // PaddleX writes `RecResizeImg`; the historic `ResRecizeImg` spelling is
    // accepted as an alias so either export loads.
    let resize = match transform(&document, "RecResizeImg") {
        Ok(resize) => resize,
        Err(error) => match transform(&document, "ResRecizeImg") {
            Ok(resize) => resize,
            Err(_) => return Err(error),
        },
    };
    let shape = i64_list(&resize["image_shape"])
        .ok_or_else(|| incompatible("Invalid OCR preprocessing/dictionary metadata"))?;
    require(
        shape == [3, 48, 320],
        "Unsupported recognition image_shape; expected [3,48,320]",
    )?;

    let post = &document["PostProcess"];
    require(
        scalar_text(&post["name"]).as_deref() == Some("CTCLabelDecode"),
        "Unsupported OCR postprocessing",
    )?;

    let dictionary = match post["character_dict"].as_vec() {
        Some(dictionary) if !dictionary.is_empty() && dictionary.len() < 100_000 => dictionary,
        _ => {
            return Err(incompatible(
                "Missing or invalid inference.yml character dictionary",
            ))
        }
    };
    // PaddleX CTCLabelDecode presumes a space class unless it is switched off.
    let use_space_char = &post["use_space_char"];
    require(
        !is_defined(use_space_char) || as_bool(use_space_char) == Some(true),
        "Unsupported CTC decoder without space character",
    )?;

    let mut characters = Vec::with_capacity(dictionary.len() + 2);
    characters.push(String::new());
    for entry in dictionary {
        let text =
            scalar_text(entry).ok_or_else(|| incompatible("Invalid CTC dictionary entry"))?;
        require(
            !text.is_empty() && !text.contains('\u{FFFD}'),
            "Invalid UTF-8 CTC dictionary entry",
        )?;
        characters.push(text);
    }
    characters.push(" ".to_string());

    Ok(RecognizerMetadata {
        height: shape[1] as usize,
        width: shape[2] as usize,
        characters,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn directory(contents: &str) -> std::path::PathBuf {
        // A monotonic counter keeps concurrently running tests in distinct
        // directories even when their contents have equal length.
        static NEXT: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
        let unique = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        let root = std::env::temp_dir().join(format!(
            "deskwaifu-ocr-metadata-{}-{unique}",
            std::process::id()
        ));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        std::fs::write(root.join("inference.yml"), contents).unwrap();
        root
    }

    const DETECTION: &str = "Global:\n  model_name: PP-OCRv6_small_det\nPreProcess:\n  transform_ops:\n  - DecodeImage: {channel_first: false, img_mode: BGR}\n  - DetResizeForTest: null\n  - NormalizeImage:\n      mean: [0.485, 0.456, 0.406]\n      std: [0.229, 0.224, 0.225]\n      scale: 1./255.\n      order: hwc\n  - ToCHWImage: null\nPostProcess:\n  name: DBPostProcess\n  thresh: 0.2\n  box_thresh: 0.45\n  unclip_ratio: 1.4\n  max_candidates: 3000\n";

    const RECOGNITION: &str = "Global:\n  model_name: PP-OCRv6_small_rec\nPreProcess:\n  transform_ops:\n  - DecodeImage: {channel_first: false, img_mode: BGR}\n  - RecResizeImg: {image_shape: [3, 48, 320]}\nPostProcess:\n  name: CTCLabelDecode\n  character_dict:\n  - 'a'\n  - 'b'\n";

    #[test]
    fn the_detector_metadata_yields_the_expected_parameters() {
        let root = directory(DETECTION);
        let metadata = load_detector(&root).unwrap();
        assert_eq!(metadata.normalization.mean, [0.485, 0.456, 0.406]);
        assert_eq!(metadata.normalization.stddev, [0.229, 0.224, 0.225]);
        assert_eq!(metadata.normalization.scale, "1./255.");
        assert_eq!(metadata.normalization.order, "hwc");
        assert_eq!(metadata.postprocess.thresh, 0.2);
        assert_eq!(metadata.postprocess.box_thresh, 0.45);
        assert_eq!(metadata.postprocess.unclip_ratio, 1.4);
        assert_eq!(metadata.postprocess.max_candidates, 3000);
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn the_dictionary_gets_a_blank_prefix_and_a_space_suffix() {
        let root = directory(RECOGNITION);
        let metadata = load_recognizer(&root).unwrap();
        assert_eq!(metadata.height, 48);
        assert_eq!(metadata.width, 320);
        assert_eq!(metadata.characters, vec!["", "a", "b", " "]);
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn the_historic_resrecizeimg_spelling_is_accepted() {
        let root = directory(&RECOGNITION.replace("RecResizeImg", "ResRecizeImg"));
        let metadata = load_recognizer(&root).unwrap();
        assert_eq!(metadata.width, 320);
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn a_float_max_candidates_is_truncated_like_yaml_cpp() {
        // `yaml-cpp` streams a real into an `int`, which truncates toward zero.
        let root = directory(&DETECTION.replace("max_candidates: 3000", "max_candidates: 3000.9"));
        let metadata = load_detector(&root).unwrap();
        assert_eq!(metadata.postprocess.max_candidates, 3000);
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn a_missing_transform_is_reported_by_name() {
        let root = directory(&DETECTION.replace("  - ToCHWImage: null\n", ""));
        match load_detector(&root) {
            Err(Error::ModelIncompatible(message)) => {
                assert!(message.contains("ToCHWImage"), "{message}")
            }
            other => panic!("expected model_incompatible, got {other:?}"),
        }
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn oversized_metadata_is_rejected_before_parsing() {
        let root = std::env::temp_dir().join(format!(
            "deskwaifu-ocr-metadata-big-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        // One byte over the 4 MiB limit, so the size check fires before parsing.
        let padding = "x".repeat(4 * 1024 * 1024 + 1);
        std::fs::write(root.join("inference.yml"), padding).unwrap();
        match load_detector(&root) {
            Err(Error::ModelIncompatible(message)) => {
                assert!(message.contains("4 MiB"), "{message}")
            }
            other => panic!("expected model_incompatible, got {other:?}"),
        }
        std::fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn a_missing_metadata_file_is_model_missing() {
        let root = std::env::temp_dir().join(format!(
            "deskwaifu-ocr-metadata-absent-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).unwrap();
        assert_eq!(load_detector(&root).unwrap_err().code(), "model_missing");
        std::fs::remove_dir_all(&root).unwrap();
    }
}
