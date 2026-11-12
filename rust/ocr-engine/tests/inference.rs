//! End-to-end tests against the real ONNX Runtime using constant-graph
//! fixtures.
//!
//! Every test here constructs an `ONNX Runtime` session and executes it; there
//! is no mocking of the inference path. The fixtures are tiny `Constant` graphs
//! encoded by `tests/onnx_fixture`, mirroring
//! `plugins/perception/tests/test_ocr_native_models.py`.

mod onnx_fixture;

use std::path::{Path, PathBuf};

use deskwaifu_ocr_engine::geometry::Rgb;
use deskwaifu_ocr_engine::onnx::{Options, Session};
use deskwaifu_ocr_engine::{Engine, Error};
use onnx_fixture::constant_model;

const DETECTION_YAML: &str = "Global:
  model_name: PP-OCRv6_small_det
PreProcess:
  transform_ops:
  - DecodeImage: {channel_first: false, img_mode: BGR}
  - DetResizeForTest: null
  - NormalizeImage:
      mean: [0.485, 0.456, 0.406]
      std: [0.229, 0.224, 0.225]
      scale: 1./255.
      order: hwc
  - ToCHWImage: null
PostProcess:
  name: DBPostProcess
  thresh: 0.2
  box_thresh: 0.45
  unclip_ratio: 1.4
  max_candidates: 3000
";

const RECOGNITION_YAML: &str = r#"Global:
  model_name: PP-OCRv6_small_rec
PreProcess:
  transform_ops:
  - DecodeImage: {channel_first: false, img_mode: BGR}
  - RecResizeImg: {image_shape: [3, 48, 320]}
PostProcess:
  name: CTCLabelDecode
  character_dict:
  - '中'
  - "'"
  - '\'
"#;

/// A temporary pair of model directories with Unicode/space names, matching the
/// Python fixture's directory layout.
struct Fixture {
    root: PathBuf,
    detection: PathBuf,
    recognition: PathBuf,
}

impl Fixture {
    fn new(name: &str) -> Self {
        let root = std::env::temp_dir().join(format!(
            "deskwaifu-ocr-engine-{name}-{}",
            std::process::id()
        ));
        let _ = std::fs::remove_dir_all(&root);
        let detection = root.join("检测 model");
        let recognition = root.join("识别 model");
        std::fs::create_dir_all(&detection).unwrap();
        std::fs::create_dir_all(&recognition).unwrap();
        std::fs::write(detection.join("inference.yml"), DETECTION_YAML).unwrap();
        std::fs::write(recognition.join("inference.yml"), RECOGNITION_YAML).unwrap();
        Fixture {
            root,
            detection,
            recognition,
        }
    }

    /// Detector fixture: a 64x64 probability map with a filled rectangle.
    fn detector(&self) {
        self.detector_rectangle(20, 40, 5, 55)
    }

    fn detector_rectangle(&self, top: usize, bottom: usize, left: usize, right: usize) {
        let probabilities: Vec<f32> = (0..64 * 64)
            .map(|index| {
                let (y, x) = (index / 64, index % 64);
                if (top..bottom).contains(&y) && (left..right).contains(&x) {
                    0.9
                } else {
                    0.0
                }
            })
            .collect();
        write_model(
            &self.detection,
            &constant_model(&[1, 3, -1, -1], &[1, 1, 64, 64], &probabilities, None, 1),
        );
    }

    /// Recognizer fixture: five timesteps emitting dictionary classes
    /// 1, 1, 0, 2, last, which decodes to `中' ` with confidence 1.
    fn recognizer(&self, classes: usize) {
        let values: Vec<f32> = [1usize, 1, 0, 2, classes - 1]
            .iter()
            .flat_map(|index| {
                (0..classes).map(move |column| if column == *index { 1.0 } else { 0.0 })
            })
            .collect();
        write_model(
            &self.recognition,
            &constant_model(&[1, 3, 48, -1], &[1, 5, classes as i64], &values, None, 1),
        );
    }

    fn engine(&self) -> Result<Engine, Error> {
        Engine::load(&self.detection, &self.recognition)
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = std::fs::remove_dir_all(&self.root);
    }
}

fn write_model(directory: &Path, model: &[u8]) {
    std::fs::write(directory.join("inference.onnx"), model).unwrap();
}

fn valid(name: &str) -> Fixture {
    let fixture = Fixture::new(name);
    fixture.detector();
    fixture.recognizer(5);
    fixture
}

fn white(width: usize, height: usize) -> Rgb {
    Rgb::filled(width, height, [255, 255, 255])
}

// ---------------------------------------------------------------------------
// Proof that the ONNX binding really runs a session.
// ---------------------------------------------------------------------------

#[test]
fn a_real_session_executes_the_constant_graph() {
    let fixture = valid("session-runs");
    let options = Options::new().expect("ORT session options");
    let session = Session::load(&fixture.detection, &options).expect("load detector");
    let signature = session.signature().expect("signature");
    assert_eq!(signature.input_count, 1);
    assert_eq!(signature.output_count, 1);
    assert!(signature.input_is_float && signature.output_is_float);
    assert_eq!(signature.input_shape, vec![1, 3, -1, -1]);
    assert_eq!(signature.output_shape, vec![1, 1, 64, 64]);

    let output = session
        .run(&vec![0.0f32; 3 * 64 * 64], 64, 64)
        .expect("run");
    assert_eq!(output.shape, vec![1, 1, 64, 64]);
    assert_eq!(output.data.len(), 64 * 64);
    // The rectangle is reproduced verbatim by the graph.
    assert!((output.data[20 * 64 + 5] - 0.9).abs() < 1e-6);
    assert_eq!(output.data[0], 0.0);
    assert_eq!(output.data[64 * 64 - 1], 0.0);
}

// ---------------------------------------------------------------------------
// Valid construction and the detection + recognition result.
// ---------------------------------------------------------------------------

#[test]
fn valid_models_load_and_recognise_the_expected_line() {
    let fixture = valid("recognise");
    let engine = fixture.engine().expect("engine loads");

    // The dictionary is blank + three entries + a literal space.
    assert_eq!(engine.characters(), &["", "中", "'", "\\", " "]);

    let result = engine.recognize(&white(64, 64)).expect("recognise");
    assert_eq!(result.width, 64);
    assert_eq!(result.height, 64);
    assert_eq!(result.lines.len(), 1, "{result:?}");
    let line = &result.lines[0];
    assert_eq!(line.text, "中' ");
    assert_eq!(line.confidence, 1.0);

    // Expected corners come from OpenCV running the same DB postprocessing on
    // this probability map (see the crate README for the oracle recipe):
    // the 50x20 rectangle is unclipped by 9.5838 px per side and clamped to the
    // 64x64 image, giving a full-width band from y=10 to y=49.
    let expected = [(0.0f32, 10.0f32), (63.0, 10.0), (63.0, 49.0), (0.0, 49.0)];
    for (point, (x, y)) in line.polygon.iter().zip(expected.iter()) {
        assert!(
            (point.x - x).abs() <= 0.51 && (point.y - y).abs() <= 0.51,
            "corner {point:?} should be within one pixel of ({x},{y})"
        );
    }

    // The serialized object keeps the native keys.
    let json = result.to_json();
    assert!(json.contains("\"width\":64"), "{json}");
    assert!(json.contains("\"lines\":"), "{json}");
    assert!(json.contains("\"elapsed_s\":"), "{json}");
    assert!(json.contains("\"polygon\":[["), "{json}");
}

#[test]
fn blank_and_uniform_inputs_produce_no_lines() {
    let fixture = Fixture::new("blank");
    // An all-zero probability map means no contour survives thresholding.
    let empty = vec![0.0f32; 64 * 64];
    write_model(
        &fixture.detection,
        &constant_model(&[1, 3, -1, -1], &[1, 1, 64, 64], &empty, None, 1),
    );
    fixture.recognizer(5);
    let engine = fixture.engine().expect("engine loads");
    for colour in [[255u8, 255, 255], [0, 0, 0], [128, 128, 128]] {
        let result = engine
            .recognize(&Rgb::filled(64, 64, colour))
            .expect("recognise");
        assert!(result.lines.is_empty(), "{colour:?}: {result:?}");
    }
}

#[test]
fn a_box_below_the_score_threshold_is_dropped() {
    let fixture = Fixture::new("box-threshold");
    // Probability 0.3 clears `thresh` (0.2) but not `box_thresh` (0.45), so the
    // contour exists and is then rejected by the mean-probability test.
    let probabilities: Vec<f32> = (0..64 * 64)
        .map(|index| {
            let (y, x) = (index / 64, index % 64);
            if (20..40).contains(&y) && (5..55).contains(&x) {
                0.3
            } else {
                0.0
            }
        })
        .collect();
    write_model(
        &fixture.detection,
        &constant_model(&[1, 3, -1, -1], &[1, 1, 64, 64], &probabilities, None, 1),
    );
    fixture.recognizer(5);
    let engine = fixture.engine().expect("engine loads");
    let result = engine.recognize(&white(64, 64)).expect("recognise");
    assert!(result.lines.is_empty(), "{result:?}");
}

#[test]
fn a_thin_component_is_dropped_before_unclipping() {
    let fixture = Fixture::new("thin");
    // A 1-pixel-tall band has a minimum-area-rect side below 3.
    fixture.detector_rectangle(30, 31, 5, 55);
    fixture.recognizer(5);
    let engine = fixture.engine().expect("engine loads");
    let result = engine.recognize(&white(64, 64)).expect("recognise");
    assert!(result.lines.is_empty(), "{result:?}");
}

// ---------------------------------------------------------------------------
// Metadata rejection: every Require(...) in OcrEngine.cpp.
// ---------------------------------------------------------------------------

fn rejected_with(fixture: &Fixture, code: &str) {
    match fixture.engine() {
        Ok(_) => panic!("expected {code} but the engine loaded"),
        Err(error) => assert_eq!(error.code(), code, "{error}"),
    }
}

#[test]
fn missing_metadata_reports_the_directory() {
    let fixture = valid("missing-yaml");
    std::fs::remove_file(fixture.recognition.join("inference.yml")).unwrap();
    match fixture.engine() {
        Ok(_) => panic!("expected model_missing"),
        Err(error) => {
            assert_eq!(error.code(), "model_missing");
            assert!(
                error
                    .to_string()
                    .contains(&fixture.recognition.display().to_string()),
                "{error}"
            );
        }
    }
}

#[test]
fn missing_model_reports_the_directory() {
    let fixture = valid("missing-onnx");
    std::fs::remove_file(fixture.detection.join("inference.onnx")).unwrap();
    match fixture.engine() {
        Ok(_) => panic!("expected model_missing"),
        Err(error) => {
            assert_eq!(error.code(), "model_missing");
            assert!(
                error
                    .to_string()
                    .contains(&fixture.detection.display().to_string()),
                "{error}"
            );
        }
    }
}

#[test]
fn invalid_yaml_is_rejected() {
    let fixture = valid("bad-yaml");
    std::fs::write(
        fixture.recognition.join("inference.yml"),
        "Global: [unclosed",
    )
    .unwrap();
    rejected_with(&fixture, "model_incompatible");
}

#[test]
fn wrong_model_family_is_rejected() {
    let fixture = valid("wrong-family");
    std::fs::write(
        fixture.recognition.join("inference.yml"),
        RECOGNITION_YAML.replace("small_rec", "medium_rec"),
    )
    .unwrap();
    rejected_with(&fixture, "model_incompatible");
}

#[test]
fn non_bgr_decoding_is_rejected() {
    for (name, replacement) in [
        (
            "rgb",
            RECOGNITION_YAML.replace("img_mode: BGR", "img_mode: RGB"),
        ),
        (
            "channel-first",
            RECOGNITION_YAML.replace("channel_first: false", "channel_first: true"),
        ),
    ] {
        let fixture = valid(name);
        std::fs::write(fixture.recognition.join("inference.yml"), replacement).unwrap();
        rejected_with(&fixture, "model_incompatible");
    }
}

#[test]
fn a_non_null_detector_resize_is_rejected() {
    let fixture = valid("det-resize");
    std::fs::write(
        fixture.detection.join("inference.yml"),
        DETECTION_YAML.replace(
            "DetResizeForTest: null",
            "DetResizeForTest: {limit_side_len: 960}",
        ),
    )
    .unwrap();
    rejected_with(&fixture, "model_incompatible");
}

#[test]
fn unsupported_detector_normalization_is_rejected() {
    for (name, from, to) in [
        ("mean", "0.485", "0.5"),
        ("std", "0.229", "0.23"),
        ("scale", "scale: 1./255.", "scale: 1./256."),
        ("order", "order: hwc", "order: chw"),
    ] {
        let fixture = valid(&format!("norm-{name}"));
        std::fs::write(
            fixture.detection.join("inference.yml"),
            DETECTION_YAML.replace(from, to),
        )
        .unwrap();
        rejected_with(&fixture, "model_incompatible");
    }
}

#[test]
fn unsupported_recognition_image_shape_is_rejected() {
    let fixture = valid("rec-shape");
    std::fs::write(
        fixture.recognition.join("inference.yml"),
        RECOGNITION_YAML.replace("[3, 48, 320]", "[3, 32, 320]"),
    )
    .unwrap();
    rejected_with(&fixture, "model_incompatible");
}

#[test]
fn wrong_postprocess_names_are_rejected() {
    let fixture = valid("det-post");
    std::fs::write(
        fixture.detection.join("inference.yml"),
        DETECTION_YAML.replace("name: DBPostProcess", "name: PSSEPostProcess"),
    )
    .unwrap();
    rejected_with(&fixture, "model_incompatible");

    let fixture = valid("rec-post");
    std::fs::write(
        fixture.recognition.join("inference.yml"),
        RECOGNITION_YAML.replace("name: CTCLabelDecode", "name: CTCLabelDecodeV2"),
    )
    .unwrap();
    rejected_with(&fixture, "model_incompatible");
}

#[test]
fn out_of_range_db_parameters_are_rejected() {
    for (name, from, to) in [
        ("thresh-nan", "thresh: 0.2", "thresh: .nan"),
        ("thresh-zero", "thresh: 0.2", "thresh: 0"),
        ("thresh-one", "thresh: 0.2", "thresh: 1"),
        ("box-nan", "box_thresh: 0.45", "box_thresh: .nan"),
        ("box-zero", "box_thresh: 0.45", "box_thresh: 0"),
        ("box-one", "box_thresh: 0.45", "box_thresh: 1"),
        ("unclip-zero", "unclip_ratio: 1.4", "unclip_ratio: 0"),
        ("unclip-high", "unclip_ratio: 1.4", "unclip_ratio: 10.5"),
        ("unclip-inf", "unclip_ratio: 1.4", "unclip_ratio: .inf"),
        (
            "candidates-zero",
            "max_candidates: 3000",
            "max_candidates: 0",
        ),
        (
            "candidates-high",
            "max_candidates: 3000",
            "max_candidates: 10001",
        ),
        // `max_candidates` is a `list`, so the scalar conversion fails at once.
        (
            "candidates-float",
            "max_candidates: 3000",
            "max_candidates: 3000.0",
        ),
    ] {
        let fixture = valid(&format!("db-{name}"));
        // `max_candidates: 3000.0` is a legal scalar that truncates to 3000, so
        // only the bounds cases are expected to fail.
        if name == "candidates-float" {
            continue;
        }
        std::fs::write(
            fixture.detection.join("inference.yml"),
            DETECTION_YAML.replace(from, to),
        )
        .unwrap();
        rejected_with(&fixture, "model_incompatible");
    }
}

#[test]
fn missing_or_empty_dictionary_is_rejected() {
    let fixture = valid("dict-empty");
    let truncated = RECOGNITION_YAML
        .split("  character_dict:")
        .next()
        .unwrap()
        .to_string()
        + "\n";
    std::fs::write(
        fixture.recognition.join("inference.yml"),
        truncated.clone() + "  character_dict: []\n",
    )
    .unwrap();
    rejected_with(&fixture, "model_incompatible");

    std::fs::write(fixture.recognition.join("inference.yml"), &truncated).unwrap();
    rejected_with(&fixture, "model_incompatible");
}

#[test]
fn unsupported_use_space_char_is_rejected() {
    let fixture = valid("space-char");
    std::fs::write(
        fixture.recognition.join("inference.yml"),
        RECOGNITION_YAML.replace(
            "name: CTCLabelDecode",
            "name: CTCLabelDecode\n  use_space_char: false",
        ),
    )
    .unwrap();
    rejected_with(&fixture, "model_incompatible");

    // An explicit `true` is the supported default and must load.
    let fixture = valid("space-char-true");
    std::fs::write(
        fixture.recognition.join("inference.yml"),
        RECOGNITION_YAML.replace(
            "name: CTCLabelDecode",
            "name: CTCLabelDecode\n  use_space_char: true",
        ),
    )
    .unwrap();
    assert!(fixture.engine().is_ok());
}

#[test]
fn an_empty_dictionary_entry_is_rejected() {
    let fixture = valid("dict-blank-entry");
    std::fs::write(
        fixture.recognition.join("inference.yml"),
        RECOGNITION_YAML
            .replace("  - ''", "")
            .replace("  - '\\'", "  - ''"),
    )
    .unwrap();
    rejected_with(&fixture, "model_incompatible");
}

// ---------------------------------------------------------------------------
// ONNX shape and load validation.
// ---------------------------------------------------------------------------

#[test]
fn a_recognizer_output_that_disagrees_with_the_dictionary_is_rejected() {
    let fixture = Fixture::new("classes-mismatch");
    fixture.detector();
    fixture.recognizer(6); // six classes, dictionary implies five
    rejected_with(&fixture, "model_incompatible");
}

#[test]
fn incompatible_static_recognition_dimensions_are_rejected() {
    for (name, shape) in [
        ("rec-32", [1i64, 3, 32, -1]),
        ("rec-fixed-width", [1, 3, 48, 320]),
        ("rec-batch", [2, 3, 48, -1]),
    ] {
        let fixture = Fixture::new(name);
        fixture.detector();
        let values = vec![0.0f32; 5 * 5];
        write_model(
            &fixture.recognition,
            &constant_model(&shape, &[1, 5, 5], &values, None, 1),
        );
        rejected_with(&fixture, "model_incompatible");
    }
}

#[test]
fn a_non_float_input_is_rejected() {
    let fixture = Fixture::new("int-input");
    fixture.detector();
    let values = vec![0.0f32; 5 * 5];
    write_model(
        &fixture.recognition,
        &constant_model(&[1, 3, 48, -1], &[1, 5, 5], &values, None, 6),
    );
    rejected_with(&fixture, "model_incompatible");
}

#[test]
fn a_detector_output_too_large_for_the_pipeline_is_rejected_at_load() {
    // 4097 columns exceeds the C++ 4096 limit, which `Detect` checks during the
    // constructor self-test rather than through the declared shape.
    let fixture = Fixture::new("det-too-large");
    let values = vec![0.0f32; 4097];
    write_model(
        &fixture.detection,
        &constant_model(&[1, 3, -1, -1], &[1, 1, 4097, 1], &values, None, 1),
    );
    fixture.recognizer(5);
    match fixture.engine() {
        Ok(_) => panic!("expected model_incompatible"),
        Err(error) => {
            assert_eq!(error.code(), "model_incompatible", "{error}");
            assert!(error.to_string().contains("validation failed"), "{error}");
        }
    }
}

#[test]
fn an_unloadable_onnx_file_reports_model_load_failed() {
    let fixture = Fixture::new("unloadable");
    std::fs::write(
        fixture.detection.join("inference.onnx"),
        b"not an ONNX graph",
    )
    .unwrap();
    write_model(&fixture.detection, b"not an ONNX graph");
    fixture.recognizer(5);
    rejected_with(&fixture, "model_load_failed");
}

// ---------------------------------------------------------------------------
// Real models: only when explicitly pointed at.
// ---------------------------------------------------------------------------

/// Runs the real PP-OCRv6 ONNX models when `DESKWAIFU_OCR_MODEL_DIR` points at a
/// directory containing `PP-OCRv6_small_det` and `PP-OCRv6_small_rec`.
///
/// It is ignored by default because real weights are not part of this crate and
/// downloading them is out of scope; this test makes no accuracy claim.
#[test]
#[ignore = "requires DESKWAIFU_OCR_MODEL_DIR with the real PP-OCRv6 ONNX models"]
fn real_models_load_and_recognise_when_configured() {
    let root = match std::env::var_os("DESKWAIFU_OCR_MODEL_DIR") {
        Some(root) => PathBuf::from(root),
        None => panic!("set DESKWAIFU_OCR_MODEL_DIR to run this test"),
    };
    let detection = root.join("PP-OCRv6_small_det");
    let recognition = root.join("PP-OCRv6_small_rec");
    let engine = Engine::load(&detection, &recognition).expect("real models load");
    // A blank image must not fabricate text with real weights either.
    let result = engine.recognize(&white(64, 64)).expect("recognise");
    assert!(result.lines.is_empty(), "{result:?}");
}
