//! Engine construction and the end-to-end recognition entry point.

use std::path::Path;

use crate::detect::{boxes, detect};
use crate::geometry::Rgb;
use crate::metadata::{self, DetectorMetadata, RecognizerMetadata};
use crate::onnx::{Options, Session, Signature};
use crate::recognition::{crop_line, recognize};
use crate::{Error, LineBox};

/// A detected and recognized text line.
#[derive(Clone, Debug, PartialEq)]
pub struct Line {
    pub text: String,
    pub confidence: f64,
    /// Top-left, top-right, bottom-right, bottom-left in image pixels.
    pub polygon: LineBox,
}

/// The result of one recognition pass.
#[derive(Clone, Debug, PartialEq)]
pub struct Recognition {
    pub width: usize,
    pub height: usize,
    pub lines: Vec<Line>,
    /// Wall-clock duration of the pass, in seconds (`elapsed_s`).
    pub elapsed_s: f64,
}

impl Recognition {
    /// The result object the C++ engine serializes, in the same key order and
    /// shape (`width`, `height`, `lines`, `elapsed_s`).
    pub fn to_json(&self) -> String {
        let mut lines = String::new();
        for (index, line) in self.lines.iter().enumerate() {
            if index > 0 {
                lines.push(',');
            }
            let points: Vec<String> = line
                .polygon
                .iter()
                .map(|point| format!("[{},{}]", number(point.x), number(point.y)))
                .collect();
            lines.push_str(&format!(
                "{{\"text\":\"{}\",\"confidence\":{},\"polygon\":[{}]}}",
                escape(&line.text),
                number(line.confidence as f32),
                points.join(",")
            ));
        }
        format!(
            "{{\"width\":{},\"height\":{},\"lines\":[{}],\"elapsed_s\":{}}}",
            self.width, self.height, lines, self.elapsed_s
        )
    }
}

/// Format a float the way Qt's JSON writer does: shortest round-trip form.
fn number(value: f32) -> String {
    if !value.is_finite() {
        return "0".to_string();
    }
    let mut text = format!("{value}");
    if text == "-0" {
        text = "0".to_string();
    }
    text
}

fn escape(text: &str) -> String {
    let mut result = String::with_capacity(text.len());
    for character in text.chars() {
        match character {
            '"' => result.push_str("\\\""),
            '\\' => result.push_str("\\\\"),
            '\n' => result.push_str("\\n"),
            '\r' => result.push_str("\\r"),
            '\t' => result.push_str("\\t"),
            '\u{08}' => result.push_str("\\b"),
            '\u{0c}' => result.push_str("\\f"),
            character if (character as u32) < 0x20 => {
                result.push_str(&format!("\\u{:04x}", character as u32));
            }
            character => result.push(character),
        }
    }
    result
}

/// A loaded detector/recognizer pair.
pub struct Engine {
    detector: Session,
    recognizer: Session,
    detector_metadata: DetectorMetadata,
    recognizer_metadata: RecognizerMetadata,
    // Retain the options so the sessions' configuration lives as long as them.
    _options: Options,
}

/// `ValidateSession`: the checks the C++ performs on declared ONNX shapes.
fn validate_session(
    signature: &Signature,
    detection: bool,
    recognition_height: i64,
    characters: usize,
) -> Result<(), Error> {
    let matches = |dimension: i64, expected: i64| dimension == -1 || dimension == expected;
    if signature.input_count != 1 || signature.output_count != 1 {
        return Err(Error::ModelIncompatible(
            "Expected one ONNX input and output".into(),
        ));
    }
    if !signature.input_is_tensor || !signature.output_is_tensor {
        return Err(Error::ModelIncompatible(
            "Expected ONNX tensor input and output".into(),
        ));
    }
    if !signature.input_is_float || !signature.output_is_float {
        return Err(Error::ModelIncompatible(
            "Expected float32 ONNX tensors".into(),
        ));
    }
    let input = &signature.input_shape;
    let supported = input.len() == 4
        && matches(input[0], 1)
        && matches(input[1], 3)
        && input[3] == -1
        && (if detection {
            input[2] == -1
        } else {
            matches(input[2], recognition_height)
        });
    if !supported {
        return Err(Error::ModelIncompatible(
            "Unsupported ONNX input shape; expected dynamic spatial detector or \
             [1,3,48,dynamic] recognizer"
                .into(),
        ));
    }
    let output = &signature.output_shape;
    if detection {
        let supported = output.len() == 4
            && matches(output[0], 1)
            && matches(output[1], 1)
            && output[2] != 0
            && output[3] != 0;
        if !supported {
            return Err(Error::ModelIncompatible(
                "Unsupported detector output shape".into(),
            ));
        }
    } else {
        let supported = output.len() == 3
            && matches(output[0], 1)
            && output[1] != 0
            && matches(output[2], characters as i64);
        if !supported {
            return Err(Error::ModelIncompatible(
                "Recognizer output classes do not match inference.yml character dictionary".into(),
            ));
        }
    }
    Ok(())
}

impl Engine {
    /// Load and validate both model directories. Mirrors the `OcrEngine`
    /// constructor: metadata, every `Require`, the sessions, their declared
    /// shapes, and a self-test pass over blank images.
    pub fn load(detection_directory: &Path, recognition_directory: &Path) -> Result<Self, Error> {
        let detector_metadata = metadata::load_detector(detection_directory)?;
        let recognizer_metadata = metadata::load_recognizer(recognition_directory)?;

        let options = Options::new().map_err(|error| Error::ModelLoadFailed(error.to_string()))?;
        let detector = load_session(detection_directory, &options)?;
        let recognizer = load_session(recognition_directory, &options)?;

        let height = recognizer_metadata.height as i64;
        validate_session(
            &detector
                .signature()
                .map_err(|error| Error::ModelLoadFailed(error.to_string()))?,
            true,
            height,
            recognizer_metadata.characters.len(),
        )?;
        validate_session(
            &recognizer
                .signature()
                .map_err(|error| Error::ModelLoadFailed(error.to_string()))?,
            false,
            height,
            recognizer_metadata.characters.len(),
        )?;

        let engine = Engine {
            detector,
            recognizer,
            detector_metadata,
            recognizer_metadata,
            _options: options,
        };
        // The C++ runs a blank 64x64 detection and a blank 48x320 recognition
        // before marking the engine available, so a dynamic-shape model that
        // only fails at run time is rejected here.
        engine.self_test().map_err(|error| {
            Error::ModelIncompatible(format!("OCR model validation failed: {error}"))
        })?;
        Ok(engine)
    }

    fn self_test(&self) -> Result<(), Error> {
        let height = self.recognizer_metadata.height;
        let width = self.recognizer_metadata.width;
        let blank = Rgb::filled(64, 64, [255, 255, 255]);
        let normalization = &self.detector_metadata.normalization;
        detect(
            &self.detector,
            &blank,
            normalization.mean,
            normalization.stddev,
        )?;
        // The C++ passes the full-size white line through `ReadLine`, so the
        // self-test also exercises the perspective warp and the resize.
        let line = Rgb::filled(width, height, [255, 255, 255]);
        let quad: LineBox = [
            crate::Point { x: 0.0, y: 0.0 },
            crate::Point {
                x: (width - 1) as f32,
                y: 0.0,
            },
            crate::Point {
                x: (width - 1) as f32,
                y: (height - 1) as f32,
            },
            crate::Point {
                x: 0.0,
                y: (height - 1) as f32,
            },
        ];
        let crop = crop_line(&line, &quad);
        recognize(
            &self.recognizer,
            &crop,
            height,
            width,
            &self.recognizer_metadata.characters,
        )?;
        Ok(())
    }

    /// Detect and recognize every line of an already decoded image.
    pub fn recognize(&self, image: &Rgb) -> Result<Recognition, Error> {
        let started = std::time::Instant::now();
        let normalization = &self.detector_metadata.normalization;
        let output = detect(
            &self.detector,
            image,
            normalization.mean,
            normalization.stddev,
        )?;
        let boxes = boxes(
            &output,
            image.width,
            image.height,
            self.detector_metadata.postprocess,
        )?;
        let height = self.recognizer_metadata.height;
        let width = self.recognizer_metadata.width;
        let mut lines = Vec::with_capacity(boxes.len());
        for line_box in boxes {
            let crop = crop_line(image, &line_box);
            let (text, confidence) = recognize(
                &self.recognizer,
                &crop,
                height,
                width,
                &self.recognizer_metadata.characters,
            )?;
            lines.push(Line {
                text,
                confidence,
                polygon: line_box,
            });
        }
        Ok(Recognition {
            width: image.width,
            height: image.height,
            lines,
            elapsed_s: started.elapsed().as_secs_f64(),
        })
    }

    /// Decode a PNG/JPEG/BMP/PNM file and recognize it.
    ///
    /// The C++ accepts absolute files up to 64 MiB with sides up to 4096; the
    /// same limits apply here so a caller cannot hand the engine a larger image
    /// than the native module accepted.
    pub fn recognize_file(&self, path: &Path) -> Result<Recognition, Error> {
        let image = decode_image(path)?;
        self.recognize(&image)
    }

    /// Detector postprocessing parameters in effect.
    pub fn detector_metadata(&self) -> &DetectorMetadata {
        &self.detector_metadata
    }

    /// Recognizer dictionary in effect, blank first and space last.
    pub fn characters(&self) -> &[String] {
        &self.recognizer_metadata.characters
    }
}

/// Maximum accepted image file size, matching the C++ 64 MiB limit.
pub const MAX_IMAGE_BYTES: u64 = 64 * 1024 * 1024;
/// Maximum accepted image side, matching the C++ 4096 pixel limit.
pub const MAX_IMAGE_SIDE: u32 = 4096;

/// Read and decode an image file to RGB8 within the native limits.
pub fn decode_image(path: &Path) -> Result<Rgb, Error> {
    if !path.is_absolute() {
        return Err(Error::Inference(
            "image must be an absolute file path, at most 64 MiB".into(),
        ));
    }
    let metadata = std::fs::metadata(path).map_err(|error| {
        Error::Inference(format!("cannot read image {}: {error}", path.display()))
    })?;
    if !metadata.is_file() || metadata.len() > MAX_IMAGE_BYTES {
        return Err(Error::Inference(
            "image must be an absolute file path, at most 64 MiB".into(),
        ));
    }
    let decoded = image::open(path)
        .map_err(|error| Error::Inference(format!("failed to decode image: {error}")))?;
    let decoded = decoded.to_rgb8();
    let (width, height) = decoded.dimensions();
    if width > MAX_IMAGE_SIDE || height > MAX_IMAGE_SIDE {
        return Err(Error::Inference(
            "invalid image or image exceeds 4096 pixels per side".into(),
        ));
    }
    Ok(Rgb {
        width: width as usize,
        height: height as usize,
        pixels: decoded.into_raw(),
    })
}

fn load_session(directory: &Path, options: &Options) -> Result<Session, Error> {
    let path = directory.join("inference.onnx");
    if !path.is_file() {
        return Err(Error::ModelMissing(format!(
            "Cannot read ONNX model: {}",
            path.display()
        )));
    }
    Session::load(directory, options)
        .map_err(|error| Error::ModelLoadFailed(format!("Cannot load {}: {error}", path.display())))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn json_matches_the_native_result_shape() {
        let recognition = Recognition {
            width: 64,
            height: 32,
            lines: vec![Line {
                text: "中' \"x\"\n".to_string(),
                confidence: 0.5,
                polygon: [
                    crate::Point { x: 0.0, y: 1.0 },
                    crate::Point { x: 63.0, y: 1.0 },
                    crate::Point { x: 63.0, y: 30.0 },
                    crate::Point { x: 0.0, y: 30.0 },
                ],
            }],
            elapsed_s: 0.0,
        };
        let json = recognition.to_json();
        assert_eq!(
            json,
            "{\"width\":64,\"height\":32,\"lines\":[{\"text\":\"中' \\\"x\\\"\\n\",\
             \"confidence\":0.5,\"polygon\":[[0,1],[63,1],[63,30],[0,30]]}],\"elapsed_s\":0}"
        );
    }

    #[test]
    fn an_empty_result_serializes_an_empty_line_array() {
        let recognition = Recognition {
            width: 1,
            height: 1,
            lines: Vec::new(),
            elapsed_s: 0.0,
        };
        assert_eq!(
            recognition.to_json(),
            "{\"width\":1,\"height\":1,\"lines\":[],\"elapsed_s\":0}"
        );
    }

    #[test]
    fn negative_zero_is_written_as_zero() {
        assert_eq!(number(-0.0), "0");
        assert_eq!(number(1.5), "1.5");
        assert_eq!(number(f32::NAN), "0");
    }
}
