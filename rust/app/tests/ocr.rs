//! OCR command surface: the model-free validation and state-machine tests, plus
//! an end-to-end run against tiny constant-graph ONNX fixtures.
//!
//! The fixture builder is a copy of `rust/ocr-engine/tests/onnx_fixture`, which
//! is a test module of another crate and therefore not importable here. It
//! encodes a single-`Constant` `ModelProto`, the same recipe the engine's own
//! tests use, so the runtime's real inference path is exercised rather than
//! mocked.

use serde_json::{json, Value};
use std::fs;
use std::io::{BufRead, BufReader, Read, Write};
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, ExitStatus, Stdio};
use std::time::{Duration, Instant};
use tempfile::{tempdir, TempDir};

const TIMEOUT: Duration = Duration::from_secs(10);

struct ChildGuard(Child);

impl ChildGuard {
    fn spawn(args: &[&std::ffi::OsStr]) -> Self {
        Self::spawn_env(args, &[])
    }

    fn spawn_env(args: &[&std::ffi::OsStr], env: &[(&str, &std::ffi::OsStr)]) -> Self {
        let mut command = Command::new(env!("CARGO_BIN_EXE_deskwaifu-rust"));
        command.args(args).stderr(Stdio::piped());
        for (key, value) in env {
            command.env(key, value);
        }
        Self(command.spawn().unwrap())
    }

    fn wait(&mut self) -> ExitStatus {
        let deadline = Instant::now() + TIMEOUT;
        loop {
            if let Some(status) = self.0.try_wait().unwrap() {
                return status;
            }
            assert!(Instant::now() < deadline, "runtime did not exit in time");
            std::thread::sleep(Duration::from_millis(10));
        }
    }

    fn stderr(&mut self) -> String {
        let mut text = String::new();
        self.0
            .stderr
            .take()
            .unwrap()
            .read_to_string(&mut text)
            .unwrap();
        text
    }
}

impl Drop for ChildGuard {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}

struct Runtime {
    child: ChildGuard,
    socket: PathBuf,
    /// The isolated `XDG_CONFIG_HOME`; the perception file lives below it.
    config: PathBuf,
    _dir: TempDir,
}

impl Runtime {
    fn start(option: &str, model: &Path) -> Self {
        Self::start_with(option, model, &[])
    }

    fn start_with(option: &str, model: &Path, extra: &[&std::ffi::OsStr]) -> Self {
        Self::start_config(option, model, extra, None)
    }

    /// Starts with an isolated `XDG_CONFIG_HOME`. `config` is the text written to
    /// `<configRoot>/deskwaifu/plugin/perception/config.json`; `None` leaves the
    /// file absent.
    fn start_config(
        option: &str,
        model: &Path,
        extra: &[&std::ffi::OsStr],
        config: Option<&str>,
    ) -> Self {
        let dir = tempdir().unwrap();
        let socket = dir.path().join("runtime.sock");
        let root = dir.path().join("config");
        if let Some(text) = config {
            let path = Self::perception_file(&root);
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(path, text).unwrap();
        }
        let mut args = vec![
            option.as_ref(),
            model.as_os_str(),
            "--socket".as_ref(),
            socket.as_os_str(),
        ];
        args.extend_from_slice(extra);
        let env = [("XDG_CONFIG_HOME", root.as_os_str())];
        let child = ChildGuard::spawn_env(&args, &env);
        let mut runtime = Self {
            child,
            socket,
            config: root,
            _dir: dir,
        };
        let deadline = Instant::now() + TIMEOUT;
        loop {
            if UnixStream::connect(&runtime.socket).is_ok() {
                return runtime;
            }
            if let Some(status) = runtime.child.0.try_wait().unwrap() {
                panic!("runtime exited ({status}): {}", runtime.child.stderr());
            }
            assert!(Instant::now() < deadline, "runtime did not bind in time");
            std::thread::sleep(Duration::from_millis(10));
        }
    }

    /// `<configRoot>/deskwaifu/plugin/perception/config.json`, the file the C++
    /// `OcrProcessor` reads through its plugin context.
    fn perception_file(root: &Path) -> PathBuf {
        root.join("deskwaifu/plugin/perception/config.json")
    }

    fn request(&self, request: Value) -> Value {
        let mut stream = UnixStream::connect(&self.socket).unwrap();
        stream.set_read_timeout(Some(TIMEOUT)).unwrap();
        stream.set_write_timeout(Some(TIMEOUT)).unwrap();
        writeln!(stream, "{request}").unwrap();
        let mut response = String::new();
        BufReader::new(stream).read_line(&mut response).unwrap();
        serde_json::from_str(&response).unwrap()
    }

    fn quit(mut self) {
        assert_eq!(self.request(json!({"command":"quit"}))["ok"], true);
        assert!(self.child.wait().success());
        assert!(!self.socket.exists());
    }
}

fn fixture() -> Option<PathBuf> {
    std::env::var_os("CUBISM_TEST_MODEL").map(|path| fs::canonicalize(path).unwrap())
}

/// Any CI-provided model suffices: the OCR commands never touch Cubism state, so
/// these tests need the runtime to start, not to render.
fn model() -> Option<PathBuf> {
    fixture()
}

// ---------------------------------------------------------------------------
// Fixture builder (copied from rust/ocr-engine/tests/onnx_fixture/mod.rs).
// ---------------------------------------------------------------------------

fn varint(mut value: u64) -> Vec<u8> {
    let mut out = Vec::new();
    while value > 127 {
        out.push((value as u8 & 127) | 128);
        value >>= 7;
    }
    out.push(value as u8);
    out
}

fn integer(field: u32, value: u64) -> Vec<u8> {
    let mut out = varint(u64::from(field << 3));
    out.extend(varint(value));
    out
}

fn bytes(field: u32, value: &[u8]) -> Vec<u8> {
    let mut out = varint(u64::from(field << 3) | 2);
    out.extend(varint(value.len() as u64));
    out.extend_from_slice(value);
    out
}

fn value_info(name: &str, shape: &[i64], element_type: u64) -> Vec<u8> {
    let mut dimensions = Vec::new();
    for (index, size) in shape.iter().enumerate() {
        let dimension = if *size > 0 {
            integer(1, *size as u64)
        } else {
            bytes(2, format!("dynamic_{index}").as_bytes())
        };
        dimensions.extend(bytes(1, &dimension));
    }
    let mut tensor_type = integer(1, element_type);
    tensor_type.extend(bytes(2, &dimensions));
    let mut out = bytes(1, name.as_bytes());
    out.extend(bytes(2, &bytes(1, &tensor_type)));
    out
}

/// A constant-graph model: IR 8, opset 13, one `Constant` node.
fn constant_model(input_shape: &[i64], output_shape: &[i64], values: &[f32]) -> Vec<u8> {
    let mut tensor = Vec::new();
    for size in output_shape {
        tensor.extend(integer(1, *size as u64));
    }
    tensor.extend(integer(2, 1)); // data_type = FLOAT
    let mut raw = Vec::with_capacity(values.len() * 4);
    for value in values {
        raw.extend(value.to_le_bytes());
    }
    tensor.extend(bytes(9, &raw)); // raw_data

    let mut attribute = bytes(1, b"value");
    attribute.extend(bytes(5, &tensor)); // type = TENSOR
    attribute.extend(integer(20, 4)); // AttributeProto::Type::TENSOR

    let mut node = bytes(2, b"output");
    node.extend(bytes(4, b"Constant"));
    node.extend(bytes(5, &attribute));

    let mut graph = bytes(1, &node);
    graph.extend(bytes(2, b"ocr-test"));
    graph.extend(bytes(11, &value_info("input", input_shape, 1)));
    graph.extend(bytes(12, &value_info("output", output_shape, 1)));

    let mut model = integer(1, 8); // IR version
    model.extend(bytes(7, &graph));
    model.extend(bytes(8, &integer(2, 13))); // opset 13
    model
}

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

/// A detector/recognizer pair whose graphs emit a fixed rectangle and the text
/// `中' `, matching the engine crate's own fixtures.
struct Models {
    detection: PathBuf,
    recognition: PathBuf,
}

impl Models {
    fn new(root: &Path) -> Self {
        let detection = root.join("det");
        let recognition = root.join("rec");
        fs::create_dir_all(&detection).unwrap();
        fs::create_dir_all(&recognition).unwrap();
        fs::write(detection.join("inference.yml"), DETECTION_YAML).unwrap();
        fs::write(recognition.join("inference.yml"), RECOGNITION_YAML).unwrap();

        // A 50x20 filled rectangle in a 64x64 probability map survives DB
        // postprocessing as one line spanning the image width.
        let probabilities: Vec<f32> = (0..64 * 64)
            .map(|index| {
                let (y, x) = (index / 64, index % 64);
                if (20..40).contains(&y) && (5..55).contains(&x) {
                    0.9
                } else {
                    0.0
                }
            })
            .collect();
        fs::write(
            detection.join("inference.onnx"),
            constant_model(&[1, 3, -1, -1], &[1, 1, 64, 64], &probabilities),
        )
        .unwrap();

        // Five timesteps emitting dictionary classes 1, 1, 0, 2, last.
        let classes = 5usize;
        let values: Vec<f32> = [1usize, 1, 0, 2, classes - 1]
            .iter()
            .flat_map(|index| {
                (0..classes).map(move |column| if column == *index { 1.0 } else { 0.0 })
            })
            .collect();
        fs::write(
            recognition.join("inference.onnx"),
            constant_model(&[1, 3, 48, -1], &[1, 5, classes as i64], &values),
        )
        .unwrap();

        Self {
            detection,
            recognition,
        }
    }

    fn args(&self) -> Vec<std::ffi::OsString> {
        vec![
            "--ocr-detection-dir".into(),
            self.detection.as_os_str().to_owned(),
            "--ocr-recognition-dir".into(),
            self.recognition.as_os_str().to_owned(),
        ]
    }
}

/// Writes a 64x64 white PNG, the input the detector fixture expects.
fn write_png(path: &Path) {
    let image = image::RgbImage::from_pixel(64, 64, image::Rgb([255, 255, 255]));
    image.save(path).unwrap();
}

fn wait_until(runtime: &Runtime, mut settled: impl FnMut(&Value) -> bool) -> Value {
    let deadline = Instant::now() + TIMEOUT;
    loop {
        let status = runtime.request(json!({"command":"get_ocr_status"}));
        if settled(&status) {
            return status;
        }
        assert!(Instant::now() < deadline, "OCR status stuck at {status}");
        std::thread::sleep(Duration::from_millis(10));
    }
}

fn wait_for_job(runtime: &Runtime, id: &str) -> Value {
    let deadline = Instant::now() + TIMEOUT;
    loop {
        let reply = runtime.request(json!({"command":"get_ocr_result","args":{"job_id":id}}));
        if reply["state"] != "pending" {
            return reply;
        }
        assert!(Instant::now() < deadline, "OCR job stuck pending: {reply}");
        std::thread::sleep(Duration::from_millis(10));
    }
}

// ---------------------------------------------------------------------------
// Configuration files (IM-05).
// ---------------------------------------------------------------------------

/// The C++ `OcrClient::ConfigureModels` defaults.
const DEFAULT_DETECTION: &str = "/usr/share/deskwaifu/ocr/models/PP-OCRv6_small_det";
const DEFAULT_RECOGNITION: &str = "/usr/share/deskwaifu/ocr/models/PP-OCRv6_small_rec";

#[test]
fn an_absent_configuration_file_uses_the_system_default_directories() {
    let Some(model) = model() else { return };
    let runtime = Runtime::start("--model", &model);
    let status = runtime.request(json!({"command":"get_ocr_status"}));
    assert_eq!(status["ok"], true, "{status}");
    assert_eq!(status["detection_model_dir"], DEFAULT_DETECTION);
    assert_eq!(status["recognition_model_dir"], DEFAULT_RECOGNITION);
    assert_eq!(status["state"], "idle");
    // Whether the defaults are installed decides availability; nothing here may
    // turn an absent configuration file into a startup failure.
    if Path::new(DEFAULT_DETECTION).is_dir() && Path::new(DEFAULT_RECOGNITION).is_dir() {
        assert_eq!(status["availability_state"], "unchecked");
        assert_eq!(status["error_code"], "");
    } else {
        assert_eq!(status["availability_state"], "unavailable");
        let message = status["error"].as_str().unwrap();
        assert!(
            message.contains(DEFAULT_DETECTION) || message.contains(DEFAULT_RECOGNITION),
            "{message}"
        );
    }

    // An unknown id stays an error and the status stays unloaded.
    assert_eq!(
        runtime.request(json!({"command":"get_ocr_result","args":{"job_id":"nope"}}))["ok"],
        false
    );
    assert_eq!(
        runtime.request(json!({"command":"cancel_ocr","args":{"job_id":"nope"}}))["ok"],
        false
    );
    runtime.quit();
}

#[test]
fn a_missing_model_directory_is_not_a_startup_failure() {
    let Some(model) = model() else { return };
    let dir = tempdir().unwrap();
    let detection = dir.path().join("missing-det");
    let recognition = dir.path().join("missing-rec");
    let config = json!({
        "ocr_detection_model_dir": detection,
        "ocr_recognition_model_dir": recognition,
    });
    let runtime = Runtime::start_config("--model", &model, &[], Some(&config.to_string()));

    // The process stayed up, reports the configured paths and disables OCR with
    // the missing path in its error.
    let status = runtime.request(json!({"command":"get_ocr_status"}));
    assert_eq!(status["detection_model_dir"], detection.to_str().unwrap());
    assert_eq!(
        status["recognition_model_dir"],
        recognition.to_str().unwrap()
    );
    assert_eq!(status["availability_state"], "unavailable");
    assert_eq!(status["available"], false);
    assert_eq!(status["error_code"], "model_missing");
    let message = status["error"].as_str().unwrap();
    assert!(message.contains(detection.to_str().unwrap()), "{message}");

    // Recognition is refused with that status text and never creates a job.
    let image = dir.path().join("image.png");
    write_png(&image);
    let reply = runtime
        .request(json!({"command":"ocr_recognize","args":{"image":image.to_str().unwrap()}}));
    assert_eq!(reply["ok"], false, "{reply}");
    assert_eq!(reply["error"], status["error"]);
    assert_eq!(
        runtime.request(json!({"command":"get_ocr_status"}))["state"],
        "idle"
    );
    runtime.quit();
}

#[test]
fn a_configuration_file_supplies_both_directories() {
    let Some(model) = model() else { return };
    if !runtime_available() {
        return;
    }
    let dir = tempdir().unwrap();
    let models = Models::new(dir.path());
    let config = json!({
        "ocr_detection_model_dir": models.detection,
        "ocr_recognition_model_dir": models.recognition,
    });
    let runtime = Runtime::start_config("--model", &model, &[], Some(&config.to_string()));

    let status = runtime.request(json!({"command":"get_ocr_status"}));
    assert_eq!(
        status["detection_model_dir"],
        models.detection.to_str().unwrap()
    );
    assert_eq!(
        status["recognition_model_dir"],
        models.recognition.to_str().unwrap()
    );
    assert_eq!(status["availability_state"], "unchecked");

    let image = dir.path().join("image.png");
    write_png(&image);
    let accepted = runtime
        .request(json!({"command":"ocr_recognize","args":{"image":image.to_str().unwrap()}}));
    assert_eq!(accepted["ok"], true, "{accepted}");
    let done = wait_for_job(&runtime, accepted["job_id"].as_str().unwrap());
    assert_eq!(done["state"], "completed", "{done}");
    assert_eq!(done["text"], "中' ");

    let status = wait_until(&runtime, |status| status["busy"] == false);
    assert_eq!(status["availability_state"], "available");
    assert_eq!(status["loaded"], true);
    assert_eq!(status["error_code"], "");
    runtime.quit();
}

#[test]
fn explicit_model_directories_override_the_configuration_file() {
    let Some(model) = model() else { return };
    let dir = tempdir().unwrap();
    let models = Models::new(dir.path());
    // A configuration file that would fail validation if it were used: the
    // explicit arguments must win before it is even read.
    let config = json!({"ocr_detection_model_dir": ""});
    let args = models.args();
    let extra: Vec<&std::ffi::OsStr> = args.iter().map(|value| value.as_os_str()).collect();
    let runtime = Runtime::start_config("--model", &model, &extra, Some(&config.to_string()));

    let status = runtime.request(json!({"command":"get_ocr_status"}));
    assert_eq!(
        status["detection_model_dir"],
        models.detection.to_str().unwrap()
    );
    assert_eq!(
        status["recognition_model_dir"],
        models.recognition.to_str().unwrap()
    );
    assert_eq!(status["availability_state"], "unchecked");
    runtime.quit();
}

#[test]
fn a_key_that_is_absent_keeps_only_the_system_default_for_that_directory() {
    let Some(model) = model() else { return };
    let dir = tempdir().unwrap();
    let detection = dir.path().join("det");
    let config = json!({"ocr_detection_model_dir": detection});
    let runtime = Runtime::start_config("--model", &model, &[], Some(&config.to_string()));
    let file = Runtime::perception_file(&runtime.config);
    assert!(file.is_file(), "{}", file.display());
    let status = runtime.request(json!({"command":"get_ocr_status"}));
    assert_eq!(status["detection_model_dir"], detection.to_str().unwrap());
    assert_eq!(status["recognition_model_dir"], DEFAULT_RECOGNITION);
    runtime.quit();
}

#[test]
fn an_invalid_configured_directory_fails_startup_naming_the_key() {
    let cases = [
        ("empty string", json!({"ocr_detection_model_dir": ""})),
        ("non-string", json!({"ocr_detection_model_dir": 42})),
        ("null", json!({"ocr_detection_model_dir": null})),
        (
            "embedded NUL",
            json!({"ocr_detection_model_dir": "/tmp/model\u{0}dir"}),
        ),
        (
            "relative path",
            json!({"ocr_detection_model_dir": "models/det"}),
        ),
        (
            "home shorthand",
            json!({"ocr_detection_model_dir": "~/models/det"}),
        ),
        (
            "recognition key",
            json!({"ocr_recognition_model_dir": "models/rec"}),
        ),
    ];
    for (name, config) in cases {
        let dir = tempdir().unwrap();
        let socket = dir.path().join("runtime.sock");
        let root = dir.path().join("config");
        let path = Runtime::perception_file(&root);
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(&path, config.to_string()).unwrap();
        let mut child = ChildGuard::spawn_env(
            &[
                "--model".as_ref(),
                Path::new("/nonexistent.model3.json").as_os_str(),
                "--socket".as_ref(),
                socket.as_os_str(),
            ],
            &[("XDG_CONFIG_HOME", root.as_os_str())],
        );
        assert!(!child.wait().success(), "{name} must fail startup");
        let stderr = child.stderr();
        let key = if config.get("ocr_recognition_model_dir").is_some() {
            "ocr_recognition_model_dir"
        } else {
            "ocr_detection_model_dir"
        };
        assert!(
            stderr.contains(key),
            "{name}: the message must name {key}: {stderr}"
        );
        assert!(
            stderr.contains("absolute directory"),
            "{name}: the message must name the problem: {stderr}"
        );
    }
}

#[test]
fn a_single_model_directory_is_rejected_at_startup() {
    let dir = tempdir().unwrap();
    let socket = dir.path().join("runtime.sock");
    let mut child = ChildGuard::spawn(&[
        "--model".as_ref(),
        Path::new("/nonexistent.model3.json").as_os_str(),
        "--socket".as_ref(),
        socket.as_os_str(),
        "--ocr-detection-dir".as_ref(),
        dir.path().as_os_str(),
    ]);
    assert!(!child.wait().success());
    assert!(child.stderr().contains("must be given together"));
}

#[test]
fn a_relative_explicit_model_directory_is_rejected_at_startup() {
    let dir = tempdir().unwrap();
    let socket = dir.path().join("runtime.sock");
    let mut child = ChildGuard::spawn(&[
        "--model".as_ref(),
        Path::new("/nonexistent.model3.json").as_os_str(),
        "--socket".as_ref(),
        socket.as_os_str(),
        "--ocr-detection-dir".as_ref(),
        Path::new("relative/det").as_os_str(),
        "--ocr-recognition-dir".as_ref(),
        Path::new("/absolute/rec").as_os_str(),
    ]);
    assert!(!child.wait().success());
    let stderr = child.stderr();
    assert!(stderr.contains("--ocr-detection-dir"), "{stderr}");
    assert!(stderr.contains("absolute directory"), "{stderr}");
}

// ---------------------------------------------------------------------------
// Argument validation: no job is created for a bad image.
// ---------------------------------------------------------------------------

#[test]
fn invalid_images_are_rejected_without_creating_a_job() {
    let Some(model) = model() else { return };
    let dir = tempdir().unwrap();
    let models = Models::new(dir.path());
    let mut args = models.args();
    let extra: Vec<&std::ffi::OsStr> = args.iter().map(|value| value.as_os_str()).collect();
    let runtime = Runtime::start_with("--model", &model, &extra);
    args.clear();

    let image = dir.path().join("image.png");
    write_png(&image);
    let directory = dir.path().join("a-directory");
    fs::create_dir_all(&directory).unwrap();
    let missing = dir.path().join("missing.png");

    let mut cases: Vec<(&str, Value)> = vec![
        ("missing field", json!({})),
        ("non-string", json!({"image": 42})),
        ("empty", json!({"image": ""})),
        ("relative", json!({"image": "image.png"})),
        ("directory", json!({"image": directory.to_str().unwrap()})),
        ("nonexistent", json!({"image": missing.to_str().unwrap()})),
    ];
    // Root reads a mode-000 file, so the unreadable case only means something
    // for an unprivileged process.
    let root = std::os::unix::fs::MetadataExt::uid(&fs::metadata("/proc/self").unwrap()) == 0;
    if !root {
        let unreadable = dir.path().join("unreadable.png");
        fs::write(&unreadable, b"secret").unwrap();
        let mut permissions = fs::metadata(&unreadable).unwrap().permissions();
        std::os::unix::fs::PermissionsExt::set_mode(&mut permissions, 0o000);
        fs::set_permissions(&unreadable, permissions).unwrap();
        cases.push(("unreadable", json!({"image": unreadable.to_str().unwrap()})));
    }
    for (name, args) in cases {
        let reply = runtime.request(json!({"command":"ocr_recognize","args":args}));
        assert_eq!(reply["ok"], false, "{name}: {reply}");
        assert_eq!(
            reply["error"], "image must be an absolute readable file path",
            "{name}"
        );
    }

    // Every rejection left the state idle, so a valid image is accepted at once.
    let status = runtime.request(json!({"command":"get_ocr_status"}));
    assert_eq!(status["state"], "idle", "{status}");
    assert_eq!(status["busy"], false);
    let accepted = runtime
        .request(json!({"command":"ocr_recognize","args":{"image":image.to_str().unwrap()}}));
    assert_eq!(accepted["ok"], true, "{accepted}");
    wait_for_job(&runtime, accepted["job_id"].as_str().unwrap());
    runtime.quit();
}

// ---------------------------------------------------------------------------
// The full state machine against a working detector/recognizer pair.
// ---------------------------------------------------------------------------

/// The end-to-end run needs a loadable ONNX Runtime, and two sessions cost a few
/// hundred milliseconds. Real CI installs the runtime (the engine crate's suite
/// already runs there), but an isolated `cargo test` on a machine without
/// `libonnxruntime.so` would otherwise fail here rather than skip. Set
/// `DESKWAIFU_SKIP_OCR_E2E=1` to skip it explicitly.
fn runtime_available() -> bool {
    if std::env::var_os("DESKWAIFU_SKIP_OCR_E2E").is_some() {
        return false;
    }
    ["/usr/lib/libonnxruntime.so", "/usr/lib64/libonnxruntime.so"]
        .iter()
        .any(|path| Path::new(path).exists())
}

#[test]
fn recognizes_retains_only_the_latest_job_and_cancels_only_a_matching_pending_one() {
    let Some(model) = model() else { return };
    if !runtime_available() {
        return;
    }
    let dir = tempdir().unwrap();
    let models = Models::new(dir.path());
    let args = models.args();
    let extra: Vec<&std::ffi::OsStr> = args.iter().map(|value| value.as_os_str()).collect();
    let runtime = Runtime::start_with("--model", &model, &extra);

    let image = dir.path().join("image.png");
    write_png(&image);
    let image = image.to_str().unwrap().to_string();
    // The fixture graphs are constant, so a large input changes nothing about the
    // text but does lengthen the real preprocessing pass. That gives the busy
    // rejection and the cancel a deterministic window to land in, as they would
    // with real weights.
    let big = dir.path().join("big.png");
    image::RgbImage::from_pixel(1024, 1024, image::Rgb([255, 255, 255]))
        .save(&big)
        .unwrap();
    let big = big.to_str().unwrap().to_string();

    // Nothing is loaded before the first accepted job, so starting the runtime
    // cannot block on two ONNX sessions.
    let status = runtime.request(json!({"command":"get_ocr_status"}));
    assert_eq!(status["availability_state"], "unchecked", "{status}");
    assert_eq!(status["loaded"], false);
    assert_eq!(status["job_id"], "");

    // Acceptance answers a job id, not a result.
    let accepted = runtime.request(json!({"command":"ocr_recognize","args":{"image":big}}));
    assert_eq!(accepted["ok"], true, "{accepted}");
    assert_eq!(accepted["state"], "pending");
    let first = accepted["job_id"].as_str().unwrap().to_string();
    assert!(!first.is_empty());

    // A second submit while the first is in flight is refused, never queued.
    let busy = runtime.request(json!({"command":"ocr_recognize","args":{"image":image}}));
    assert_eq!(busy["ok"], false, "{busy}");
    assert_eq!(busy["error"], "OCR is busy");

    let done = wait_for_job(&runtime, &first);
    assert_eq!(done["state"], "completed", "{done}");
    assert_eq!(done["ok"], true);
    assert_eq!(done["cancelled"], false);
    // `text` is `OcrProcessor`'s newline join of the line texts.
    let text = done["text"].as_str().unwrap().to_string();
    assert_eq!(text, "中' ");
    // The native result object keeps the engine keys; `width`/`height` are the
    // decoded source image's dimensions, not the detector input's.
    let result = &done["response"]["result"];
    assert_eq!(result["width"], 1024);
    assert_eq!(result["height"], 1024);
    let lines = result["lines"].as_array().unwrap();
    assert_eq!(lines.len(), 1);
    assert_eq!(lines[0]["text"], "中' ");
    assert_eq!(lines[0]["confidence"], 1.0);
    assert_eq!(lines[0]["polygon"].as_array().unwrap().len(), 4);
    assert_eq!(
        lines
            .iter()
            .map(|line| line["text"].as_str().unwrap())
            .collect::<Vec<_>>()
            .join("\n"),
        text
    );
    assert_eq!(done["response"]["protocol"], 1);
    assert_eq!(done["response"]["id"], first);

    // The engine is now loaded and stays available; the job is no longer busy.
    let status = wait_until(&runtime, |status| status["busy"] == false);
    assert_eq!(status["availability_state"], "available");
    assert_eq!(status["loaded"], true);
    assert_eq!(status["error_code"], "");
    assert_eq!(status["job_id"], first);

    // A finished job cannot be cancelled and an unknown id fails both commands.
    let late = runtime.request(json!({"command":"cancel_ocr","args":{"job_id":first}}));
    assert_eq!(late["ok"], false, "{late}");
    assert_eq!(late["error"], "no matching pending OCR job");
    // `get_ocr_result` keeps the receipt-retention text; `cancel_ocr` uses the
    // cancel-specific one, exactly as the C++ `Result`/`Cancel` do.
    let reply = runtime.request(json!({"command":"get_ocr_result","args":{"job_id":"unknown"}}));
    assert_eq!(reply["ok"], false, "{reply}");
    assert_eq!(
        reply["error"],
        "unknown OCR job; only the most recent job is retained"
    );
    let reply = runtime.request(json!({"command":"cancel_ocr","args":{"job_id":"unknown"}}));
    assert_eq!(reply["ok"], false, "{reply}");
    assert_eq!(reply["error"], "no matching pending OCR job");
    for args in [json!({}), json!({"job_id": 7})] {
        let reply = runtime.request(json!({"command":"get_ocr_result","args":args}));
        assert_eq!(reply["ok"], false, "{reply}");
    }

    // A new accepted job replaces the previous receipt outright.
    let accepted = runtime.request(json!({"command":"ocr_recognize","args":{"image":image}}));
    assert_eq!(accepted["ok"], true, "{accepted}");
    let second = accepted["job_id"].as_str().unwrap().to_string();
    assert_ne!(second, first);
    let stale = runtime.request(json!({"command":"get_ocr_result","args":{"job_id":first}}));
    assert_eq!(stale["ok"], false, "{stale}");
    let done = wait_for_job(&runtime, &second);
    assert_eq!(done["state"], "completed");
    assert_eq!(done["text"], "中' ");

    // Cancel a job that is genuinely pending, then prove its late completion is
    // discarded rather than becoming the retained result. The retry loop only
    // guards against an unusually fast machine; on the fixture the cancel lands
    // while the worker is still pre-processing the large image.
    let deadline = Instant::now() + TIMEOUT;
    let third = loop {
        let accepted = runtime.request(json!({"command":"ocr_recognize","args":{"image":big}}));
        assert_eq!(accepted["ok"], true, "{accepted}");
        let id = accepted["job_id"].as_str().unwrap().to_string();
        let cancelled = runtime.request(json!({"command":"cancel_ocr","args":{"job_id":id}}));
        if cancelled["ok"] == true {
            assert_eq!(cancelled["job_id"], id);
            break id;
        }
        assert!(
            Instant::now() < deadline,
            "no job stayed pending long enough to cancel: {cancelled}"
        );
        assert_eq!(wait_for_job(&runtime, &id)["state"], "completed");
    };
    let after = runtime.request(json!({"command":"get_ocr_result","args":{"job_id":third}}));
    assert_eq!(after["state"], "failed", "{after}");
    assert_eq!(after["cancelled"], true);
    assert_eq!(after["response"]["error"], "OCR request cancelled");
    assert_eq!(after["text"], Value::Null);
    // Cancelling twice reports nothing more was cancelled.
    assert_eq!(
        runtime.request(json!({"command":"cancel_ocr","args":{"job_id":third}}))["error"],
        "no matching pending OCR job"
    );

    // Wait for the worker's late completion: it must not resurrect the text and
    // must release the busy flag so another job can be accepted.
    let status = wait_until(&runtime, |status| status["busy"] == false);
    assert_eq!(status["state"], "failed");
    let after = runtime.request(json!({"command":"get_ocr_result","args":{"job_id":third}}));
    assert_eq!(after["state"], "failed");
    assert_eq!(after["cancelled"], true);
    assert_eq!(after["text"], Value::Null);
    assert_eq!(after["response"]["error"], "OCR request cancelled");

    let accepted = runtime.request(json!({"command":"ocr_recognize","args":{"image":image}}));
    assert_eq!(accepted["ok"], true, "{accepted}");
    let fourth = accepted["job_id"].as_str().unwrap();
    assert_eq!(wait_for_job(&runtime, fourth)["state"], "completed");
    runtime.quit();
}

#[test]
fn a_model_directory_that_cannot_load_reports_the_failure_without_crashing() {
    let Some(model) = model() else { return };
    if !runtime_available() {
        return;
    }
    let dir = tempdir().unwrap();
    let models = Models::new(dir.path());
    fs::remove_file(models.detection.join("inference.onnx")).unwrap();
    let args = models.args();
    let extra: Vec<&std::ffi::OsStr> = args.iter().map(|value| value.as_os_str()).collect();
    let runtime = Runtime::start_with("--model", &model, &extra);

    let image = dir.path().join("image.png");
    write_png(&image);
    let accepted = runtime
        .request(json!({"command":"ocr_recognize","args":{"image":image.to_str().unwrap()}}));
    assert_eq!(accepted["ok"], true, "{accepted}");
    let done = wait_for_job(&runtime, accepted["job_id"].as_str().unwrap());
    assert_eq!(done["state"], "failed", "{done}");
    assert_eq!(done["response"]["ok"], false);
    let message = done["response"]["error"].as_str().unwrap();
    assert!(message.contains("inference.onnx"), "{message}");

    let status = wait_until(&runtime, |status| status["busy"] == false);
    assert_eq!(status["availability_state"], "unavailable");
    assert_eq!(status["loaded"], false);
    assert_eq!(status["error_code"], "model_missing");

    // The model is not retried, so a second submit keeps failing clearly.
    let again = runtime
        .request(json!({"command":"ocr_recognize","args":{"image":image.to_str().unwrap()}}));
    assert_eq!(again["ok"], false, "{again}");
    runtime.quit();
}
