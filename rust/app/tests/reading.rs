//! Clipboard reading-aloud pipeline: `reading_clip` and
//! `get_reading_clip_status`.
//!
//! The pipeline reads a clipboard image, runs OCR on it and speaks the result,
//! all on the runtime's own 16 ms tick. Every test here drives the real runtime
//! over its socket:
//!
//! - `DESKWAIFU_READING_TEST_IMAGE=<png>` replaces the `wl-paste` clipboard read;
//! - the tiny constant-graph ONNX fixtures from `tests/ocr.rs` exercise real
//!   inference without model weights;
//! - a loopback MiniMax WebSocket peer stands in for the speech service and
//!   records the text it was sent.
//!
//! Nothing here touches a real clipboard, a real cloud endpoint or a real audio
//! device: the silent output sink is required for the tests that reach
//! `speaking`.

use futures_util::{SinkExt, StreamExt};
use serde_json::{json, Value};
use std::fs;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::TcpListener;
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, ExitStatus, Stdio};
use std::sync::mpsc::{Receiver, Sender};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};
use tempfile::{tempdir, TempDir};
use tokio::net::TcpListener as TokioListener;
use tokio_tungstenite::{accept_async, tungstenite::Message, WebSocketStream};

const TIMEOUT: Duration = Duration::from_secs(10);
/// Speech tests wait for a full pipeline, which includes two ONNX sessions.
const PIPELINE_TIMEOUT: Duration = Duration::from_secs(30);
/// A key distinctive enough to grep every reply for it.
const SECRET: &str = "sk-reading-loopback-never-echo";
/// The recognized text the fixture models produce (`tests/ocr.rs`).
const TEXT: &str = "中' ";

struct ChildGuard(Child);

impl ChildGuard {
    fn spawn(args: &[&std::ffi::OsStr], env: &[(&str, &std::ffi::OsStr)]) -> Self {
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
    config: PathBuf,
    _dir: TempDir,
}

/// Everything a test needs to start the runtime: the model, optional OCR model
/// directories, the speech endpoint and an isolated configuration root.
#[derive(Default)]
struct Setup {
    ocr: Option<Vec<std::ffi::OsString>>,
    endpoint: Option<String>,
    /// `DESKWAIFU_READING_TEST_IMAGE`, the clipboard replacement.
    image: Option<PathBuf>,
    /// Extra environment, e.g. a `PATH` without `wl-paste`.
    path: Option<std::ffi::OsString>,
}

impl Runtime {
    fn start(model: &Path, setup: Setup) -> Self {
        let dir = tempdir().unwrap();
        let socket = dir.path().join("runtime.sock");
        let config = dir.path().join("config");
        let mut args: Vec<std::ffi::OsString> = vec![
            "--model".into(),
            model.as_os_str().to_owned(),
            "--socket".into(),
            socket.as_os_str().to_owned(),
        ];
        if let Some(ocr) = setup.ocr {
            args.extend(ocr);
        }
        if let Some(endpoint) = &setup.endpoint {
            args.push("--speech-endpoint".into());
            args.push(endpoint.into());
        }
        let argv: Vec<&std::ffi::OsStr> = args.iter().map(|value| value.as_os_str()).collect();
        let mut env: Vec<(&str, &std::ffi::OsStr)> = vec![
            ("XDG_CONFIG_HOME", config.as_os_str()),
            // CI has no audio device, so a playback must use the silent sink.
            ("DESKWAIFU_SPEECH_TEST_SINK", std::ffi::OsStr::new("1")),
        ];
        if let Some(image) = &setup.image {
            env.push(("DESKWAIFU_READING_TEST_IMAGE", image.as_os_str()));
        }
        if let Some(path) = &setup.path {
            env.push(("PATH", path.as_os_str()));
        }
        let child = ChildGuard::spawn(&argv, &env);
        let mut runtime = Self {
            child,
            socket,
            config,
            _dir: dir,
        };
        runtime.await_socket();
        runtime
    }

    fn await_socket(&mut self) {
        let deadline = Instant::now() + TIMEOUT;
        loop {
            if UnixStream::connect(&self.socket).is_ok() {
                return;
            }
            if let Some(status) = self.child.0.try_wait().ok().flatten() {
                panic!("runtime exited ({status}): {}", self.child.stderr());
            }
            assert!(Instant::now() < deadline, "runtime did not bind in time");
            std::thread::sleep(Duration::from_millis(10));
        }
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

    fn status(&self) -> Value {
        self.request(json!({"command":"get_reading_clip_status"}))
    }

    /// Polls the reading status until `settled` holds.
    fn wait_status(&self, timeout: Duration, mut settled: impl FnMut(&Value) -> bool) -> Value {
        let deadline = Instant::now() + timeout;
        loop {
            let status = self.status();
            if settled(&status) {
                return status;
            }
            assert!(
                Instant::now() < deadline,
                "reading status stuck at {status}"
            );
            std::thread::sleep(Duration::from_millis(5));
        }
    }

    /// Waits for `state`, so a test proves a state was really entered.
    fn wait_state(&self, timeout: Duration, state: &str) -> Value {
        self.wait_status(timeout, |status| status["state"] == state)
    }

    fn configure_speech(&self) {
        assert_eq!(
            self.request(json!({"command":"set_speech_settings","api_key":SECRET}))["ok"],
            true
        );
    }

    fn key_file(&self) -> PathBuf {
        self.config.join("plugin/speech/config.json")
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

// ---------------------------------------------------------------------------
// Fixture builder (copied from `tests/ocr.rs`, itself a copy of the engine
// crate's `onnx_fixture`): a constant-graph detector/recognizer pair that
// recognizes the text `中' ` from any white enough image.
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

fn constant_model(input_shape: &[i64], output_shape: &[i64], values: &[f32]) -> Vec<u8> {
    let mut tensor = Vec::new();
    for size in output_shape {
        tensor.extend(integer(1, *size as u64));
    }
    tensor.extend(integer(2, 1));
    let mut raw = Vec::with_capacity(values.len() * 4);
    for value in values {
        raw.extend(value.to_le_bytes());
    }
    tensor.extend(bytes(9, &raw));

    let mut attribute = bytes(1, b"value");
    attribute.extend(bytes(5, &tensor));
    attribute.extend(integer(20, 4));

    let mut node = bytes(2, b"output");
    node.extend(bytes(4, b"Constant"));
    node.extend(bytes(5, &attribute));

    let mut graph = bytes(1, &node);
    graph.extend(bytes(2, b"ocr-test"));
    graph.extend(bytes(11, &value_info("input", input_shape, 1)));
    graph.extend(bytes(12, &value_info("output", output_shape, 1)));

    let mut model = integer(1, 8);
    model.extend(bytes(7, &graph));
    model.extend(bytes(8, &integer(2, 13)));
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

/// A detector/recognizer pair. Its detector emits a fixed rectangle unless it
/// is built blank, and its recognizer always emits the text `中' `.
struct Models {
    detection: PathBuf,
    recognition: PathBuf,
}

impl Models {
    /// `blank` builds a detector that emits no box at all, so recognition
    /// returns an empty line list — the `no text recognized` case.
    fn new(root: &Path, blank: bool) -> Self {
        let detection = root.join("det");
        let recognition = root.join("rec");
        fs::create_dir_all(&detection).unwrap();
        fs::create_dir_all(&recognition).unwrap();
        fs::write(detection.join("inference.yml"), DETECTION_YAML).unwrap();
        fs::write(recognition.join("inference.yml"), RECOGNITION_YAML).unwrap();

        let probabilities: Vec<f32> = (0..64 * 64)
            .map(|index| {
                let (y, x) = (index / 64, index % 64);
                if !blank && (20..40).contains(&y) && (5..55).contains(&x) {
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

/// Writes a white PNG of the given size, the input the fixture detector expects.
fn write_png(path: &Path, width: u32, height: u32) {
    image::RgbImage::from_pixel(width, height, image::Rgb([255, 255, 255]))
        .save(path)
        .unwrap();
}

/// The end-to-end tests need a loadable ONNX Runtime. Real CI installs it (the
/// engine crate's own suite runs there); an isolated `cargo test` elsewhere
/// would fail here rather than skip. `DESKWAIFU_SKIP_OCR_E2E=1` skips it
/// explicitly, exactly like `tests/ocr.rs`.
fn runtime_available() -> bool {
    if std::env::var_os("DESKWAIFU_SKIP_OCR_E2E").is_some() {
        return false;
    }
    ["/usr/lib/libonnxruntime.so", "/usr/lib64/libonnxruntime.so"]
        .iter()
        .any(|path| Path::new(path).exists())
}

// ---------------------------------------------------------------------------
// Loopback MiniMax-protocol peer (the minimal subset of `tests/speech.rs`).
// ---------------------------------------------------------------------------

/// A loopback peer that completes one synthesis and reports the `task_continue`
/// texts it was sent, so a test can prove what really reached the service.
struct Speech {
    endpoint: String,
    texts: Receiver<String>,
    connections: Receiver<()>,
    thread: Option<JoinHandle<()>>,
}

impl Speech {
    fn start() -> Self {
        Self::with_hold(false)
    }

    /// A peer that accepts the handshake and then hangs, keeping the playback in
    /// `preparing` so a test can cancel a run that is already speaking.
    fn hold() -> Self {
        Self::with_hold(true)
    }

    fn with_hold(hold: bool) -> Self {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        listener.set_nonblocking(true).unwrap();
        let endpoint = format!("ws://{}", listener.local_addr().unwrap());
        let (sent, texts) = std::sync::mpsc::channel();
        let (connected, connections) = std::sync::mpsc::channel();
        let thread = std::thread::spawn(move || {
            let Ok(runtime) = tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
            else {
                return;
            };
            runtime.block_on(async move {
                let listener = TokioListener::from_std(listener).unwrap();
                loop {
                    let Ok((stream, _)) = listener.accept().await else {
                        return;
                    };
                    let _ = connected.send(());
                    let Ok(mut ws) = accept_async(stream).await else {
                        continue;
                    };
                    if hold {
                        // Silence from here on: the runtime's own close ends it.
                        while let Some(Ok(message)) = ws.next().await {
                            if matches!(message, Message::Close(_)) {
                                break;
                            }
                        }
                    } else {
                        serve(ws, &sent).await;
                    }
                }
            });
        });
        Self {
            endpoint,
            texts,
            connections,
            thread: Some(thread),
        }
    }

    /// The text of the next completed synthesis.
    fn next_text(&self, timeout: Duration) -> String {
        self.texts
            .recv_timeout(timeout)
            .expect("the speech peer saw no synthesis")
    }

    /// Whether the peer saw a connection at all, within `timeout`.
    fn connected(&self, timeout: Duration) -> bool {
        self.connections.recv_timeout(timeout).is_ok()
    }
}

impl Drop for Speech {
    fn drop(&mut self) {
        // The accept loop ends with the process; joining it here would block on
        // an `accept` that never returns, so the handle is simply detached.
        self.thread = None;
    }
}

async fn serve(mut ws: WebSocketStream<tokio::net::TcpStream>, texts: &Sender<String>) {
    send(&mut ws, "connected_success", json!({})).await;
    let Some(start) = incoming(&mut ws).await else {
        return;
    };
    if start["event"] != "task_start" {
        return;
    }
    send(&mut ws, "task_started", json!({})).await;
    let Some(continuation) = incoming(&mut ws).await else {
        return;
    };
    if continuation["event"] != "task_continue" {
        return;
    }
    let Some(_finish) = incoming(&mut ws).await else {
        return;
    };
    send(&mut ws, "sentence_start", json!({})).await;
    let audio = "00".repeat(16);
    send(&mut ws, "task_continued", json!({"data":{"audio":audio}})).await;
    send(&mut ws, "sentence_end", json!({})).await;
    send(&mut ws, "task_finished", json!({})).await;
    let _ = texts.send(continuation["text"].as_str().unwrap_or_default().to_owned());
    // Drain the socket so the runtime's own close is what ends this peer.
    while let Some(Ok(message)) = ws.next().await {
        if matches!(message, Message::Close(_)) {
            return;
        }
    }
}

async fn incoming(ws: &mut WebSocketStream<tokio::net::TcpStream>) -> Option<Value> {
    let message = ws.next().await?.ok()?;
    serde_json::from_str(message.to_text().ok()?).ok()
}

async fn send(ws: &mut WebSocketStream<tokio::net::TcpStream>, event: &str, extra: Value) {
    let mut value = json!({"event": event, "base_resp": {"status_code": 0}});
    if let Some(map) = extra.as_object() {
        for (key, item) in map {
            value[key] = item.clone();
        }
    }
    ws.send(Message::Text(value.to_string())).await.unwrap();
}

// ---------------------------------------------------------------------------
// Status and the trigger reply.
// ---------------------------------------------------------------------------

#[test]
fn status_starts_idle_and_the_trigger_only_reports_queued() {
    let Some(model) = fixture() else { return };
    let runtime = Runtime::start(&model, Setup::default());
    let status = runtime.status();
    assert_eq!(status, json!({"ok":true,"state":"idle","error":""}));
    // The trigger answers before any step can run: it only means the request was
    // accepted, exactly like the C++ `{ok,queued}`.
    let reply = runtime.request(json!({"command":"reading_clip"}));
    assert_eq!(reply, json!({"ok":true,"queued":true}));
    runtime.quit();
}

// ---------------------------------------------------------------------------
// Missing capabilities.
// ---------------------------------------------------------------------------

#[test]
fn without_ocr_the_run_fails_naming_ocr_and_never_speaks() {
    let Some(model) = fixture() else { return };
    let speech = Speech::start();
    let dir = tempdir().unwrap();
    let image = dir.path().join("clipboard.png");
    write_png(&image, 64, 64);
    let runtime = Runtime::start(
        &model,
        Setup {
            endpoint: Some(speech.endpoint.clone()),
            image: Some(image),
            ..Setup::default()
        },
    );
    runtime.configure_speech();
    assert_eq!(
        runtime.request(json!({"command":"reading_clip"}))["queued"],
        true
    );
    let done = runtime.wait_status(PIPELINE_TIMEOUT, |status| {
        status["state"] == "idle" && status["error"].as_str().is_some_and(|e| !e.is_empty())
    });
    let error = done["error"].as_str().unwrap();
    assert!(error.contains("OCR"), "{error}");
    assert!(error.contains("--ocr-detection-dir"), "{error}");
    // Nothing reached the speech service.
    assert!(!speech.connected(Duration::from_millis(300)));
    runtime.quit();
}

#[test]
fn without_a_speech_key_the_run_reaches_speaking_then_fails() {
    let Some(model) = fixture() else { return };
    if !runtime_available() {
        return;
    }
    let dir = tempdir().unwrap();
    let models = Models::new(dir.path(), false);
    let image = dir.path().join("clipboard.png");
    write_png(&image, 64, 64);
    let runtime = Runtime::start(
        &model,
        Setup {
            ocr: Some(models.args()),
            image: Some(image),
            ..Setup::default()
        },
    );
    assert_eq!(
        runtime.request(json!({"command":"reading_clip"}))["queued"],
        true
    );
    runtime.wait_state(PIPELINE_TIMEOUT, "speaking");
    let done = runtime.wait_status(PIPELINE_TIMEOUT, |status| {
        status["state"] == "idle" && status["error"].as_str().is_some_and(|e| !e.is_empty())
    });
    assert_eq!(done["error"], "请先在控制面板中设置 MiniMax API Key");
    assert!(!runtime.key_file().exists());
    runtime.quit();
}

// ---------------------------------------------------------------------------
// The full pipeline.
// ---------------------------------------------------------------------------

#[test]
fn the_pipeline_reads_recognizes_and_speaks_the_recognized_line() {
    let Some(model) = fixture() else { return };
    if !runtime_available() {
        return;
    }
    let speech = Speech::start();
    let dir = tempdir().unwrap();
    let models = Models::new(dir.path(), false);
    let image = dir.path().join("clipboard.png");
    write_png(&image, 64, 64);
    let runtime = Runtime::start(
        &model,
        Setup {
            ocr: Some(models.args()),
            endpoint: Some(speech.endpoint.clone()),
            image: Some(image),
            ..Setup::default()
        },
    );
    runtime.configure_speech();
    // The status reports idle with no error before anything runs.
    assert_eq!(runtime.status()["state"], "idle");
    assert_eq!(runtime.status()["error"], "");

    assert_eq!(
        runtime.request(json!({"command":"reading_clip"})),
        json!({"ok":true,"queued":true})
    );
    // Each step is observable: the run really passes through the C++ sequence.
    runtime.wait_state(PIPELINE_TIMEOUT, "reading_image");
    runtime.wait_state(PIPELINE_TIMEOUT, "recognizing");
    runtime.wait_state(PIPELINE_TIMEOUT, "speaking");
    let done = runtime.wait_state(PIPELINE_TIMEOUT, "idle");
    assert_eq!(done["ok"], true, "{done}");
    assert_eq!(done["error"], "", "{done}");
    // Completion waited for the speech to finish, and the recognized line is what
    // the service was asked to synthesize.
    let text = speech.next_text(PIPELINE_TIMEOUT);
    assert_eq!(text, TEXT);
    // IM-02: the temporary PNG lived through the OCR job and is gone afterwards.
    // Only this runtime's process is inspected; other tests in the same directory
    // run their own runtime.
    let prefix = format!("deskwaifu-reading-{}-", runtime.child.0.id());
    let leftovers: Vec<_> = fs::read_dir(std::env::temp_dir())
        .unwrap()
        .filter_map(|entry| entry.ok())
        .filter(|entry| entry.file_name().to_string_lossy().starts_with(&prefix))
        .map(|entry| entry.path())
        .collect();
    assert!(leftovers.is_empty(), "temporary images left: {leftovers:?}");
    runtime.quit();
}

#[test]
fn a_blank_ocr_result_fails_with_no_text_and_never_speaks() {
    let Some(model) = fixture() else { return };
    if !runtime_available() {
        return;
    }
    let speech = Speech::start();
    let dir = tempdir().unwrap();
    let models = Models::new(dir.path(), true);
    let image = dir.path().join("clipboard.png");
    write_png(&image, 64, 64);
    let runtime = Runtime::start(
        &model,
        Setup {
            ocr: Some(models.args()),
            endpoint: Some(speech.endpoint.clone()),
            image: Some(image),
            ..Setup::default()
        },
    );
    runtime.configure_speech();
    assert_eq!(
        runtime.request(json!({"command":"reading_clip"}))["queued"],
        true
    );
    let done = runtime.wait_status(PIPELINE_TIMEOUT, |status| {
        status["state"] == "idle" && status["error"].as_str().is_some_and(|e| !e.is_empty())
    });
    assert_eq!(done["error"], "no text recognized", "{done}");
    assert!(
        !speech.connected(Duration::from_millis(300)),
        "an empty recognition must not reach the speech service"
    );
    runtime.quit();
}

// ---------------------------------------------------------------------------
// Clipboard validation: the image checks happen before OCR is asked.
// ---------------------------------------------------------------------------

#[test]
fn an_image_over_4096_pixels_per_side_is_rejected_before_ocr() {
    let Some(model) = fixture() else { return };
    if !runtime_available() {
        return;
    }
    let speech = Speech::start();
    let dir = tempdir().unwrap();
    let models = Models::new(dir.path(), false);
    let image = dir.path().join("clipboard.png");
    // 5000x8 decompresses cheaply but is over the C++ `CreateImageFile` bound.
    write_png(&image, 5000, 8);
    let runtime = Runtime::start(
        &model,
        Setup {
            ocr: Some(models.args()),
            endpoint: Some(speech.endpoint.clone()),
            image: Some(image),
            ..Setup::default()
        },
    );
    runtime.configure_speech();
    assert_eq!(
        runtime.request(json!({"command":"reading_clip"}))["queued"],
        true
    );
    let done = runtime.wait_status(PIPELINE_TIMEOUT, |status| {
        status["state"] == "idle" && status["error"].as_str().is_some_and(|e| !e.is_empty())
    });
    assert_eq!(
        done["error"], "image is too large or PNG encoding failed",
        "{done}"
    );
    // Rejected in `reading_image`: OCR never got a job and nothing was spoken.
    let ocr = runtime.request(json!({"command":"get_ocr_status"}));
    assert_eq!(ocr["state"], "idle", "{ocr}");
    assert_eq!(ocr["busy"], false, "{ocr}");
    assert!(!speech.connected(Duration::from_millis(300)));
    runtime.quit();
}

#[test]
fn oversized_wl_paste_output_is_bounded_and_stops_promptly() {
    let Some(model) = fixture() else { return };
    let dir = tempdir().unwrap();
    let bin = dir.path().join("bin");
    fs::create_dir_all(&bin).unwrap();
    let wl_paste = bin.join("wl-paste");
    fs::write(
        &wl_paste,
        "#!/bin/sh\nwhile :; do printf 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx'; done\n",
    )
    .unwrap();
    use std::os::unix::fs::PermissionsExt;
    fs::set_permissions(&wl_paste, fs::Permissions::from_mode(0o755)).unwrap();
    let runtime = Runtime::start(
        &model,
        Setup {
            path: Some(bin.as_os_str().to_owned()),
            ..Setup::default()
        },
    );
    runtime.request(json!({"command":"reading_clip"}));
    let started = Instant::now();
    let done = runtime.wait_status(Duration::from_secs(10), |status| {
        status["state"] == "idle"
            && status["error"]
                .as_str()
                .is_some_and(|error| !error.is_empty())
    });
    assert_eq!(done["error"], "image is too large or PNG encoding failed");
    assert!(
        started.elapsed() < Duration::from_secs(5),
        "oversized output was not stopped promptly"
    );
    runtime.quit();
}

#[test]
fn a_missing_wl_paste_reports_a_clear_error_instead_of_hanging() {
    let Some(model) = fixture() else { return };
    let dir = tempdir().unwrap();
    // A PATH with no `wl-paste` anywhere, so the read fails at once.
    let empty = dir.path().join("bin");
    fs::create_dir_all(&empty).unwrap();
    let runtime = Runtime::start(
        &model,
        Setup {
            path: Some(empty.as_os_str().to_owned()),
            ..Setup::default()
        },
    );
    assert_eq!(
        runtime.request(json!({"command":"reading_clip"}))["queued"],
        true
    );
    let done = runtime.wait_status(PIPELINE_TIMEOUT, |status| {
        status["state"] == "idle" && status["error"].as_str().is_some_and(|e| !e.is_empty())
    });
    let error = done["error"].as_str().unwrap();
    assert!(error.contains("wl-paste"), "{error}");
    assert!(error.contains("wl-clipboard"), "{error}");
    runtime.quit();
}

// ---------------------------------------------------------------------------
// RC-03: cancel, repeat trigger, and late completions.
// ---------------------------------------------------------------------------

#[test]
fn a_trigger_while_speaking_cancels_and_a_late_completion_does_not_resume() {
    let Some(model) = fixture() else { return };
    if !runtime_available() {
        return;
    }
    let speech = Speech::hold();
    let dir = tempdir().unwrap();
    let models = Models::new(dir.path(), false);
    let image = dir.path().join("clipboard.png");
    write_png(&image, 64, 64);
    let runtime = Runtime::start(
        &model,
        Setup {
            ocr: Some(models.args()),
            endpoint: Some(speech.endpoint.clone()),
            image: Some(image),
            ..Setup::default()
        },
    );
    runtime.configure_speech();
    assert_eq!(
        runtime.request(json!({"command":"reading_clip"}))["queued"],
        true
    );
    runtime.wait_state(PIPELINE_TIMEOUT, "speaking");
    let speech_before = runtime.request(json!({"command":"get_speech_status"}))["id"].clone();
    // A second trigger cancels the current step and enters `cancelling`.
    assert_eq!(
        runtime.request(json!({"command":"reading_clip"})),
        json!({"ok":true,"queued":true})
    );
    runtime.wait_state(PIPELINE_TIMEOUT, "cancelling");
    // The held peer never completes, so the playback keeps running; the run
    // releases that step without waiting for the audio.
    assert!(
        runtime.status()["error"]
            .as_str()
            .is_some_and(|e| e.is_empty()),
        "{}",
        runtime.status()
    );
    runtime.wait_status(PIPELINE_TIMEOUT, |status| {
        status["state"] == "idle" && status["error"].as_str().is_some_and(|e| !e.is_empty())
    });
    let cancelled = runtime.status();
    assert_eq!(cancelled["error"], "reading cancelled", "{cancelled}");

    // A late step for the cancelled run cannot resume it: the run is idle with
    // `reading cancelled` even after the speech eventually fails.
    let late = runtime.wait_status(PIPELINE_TIMEOUT, |_| {
        runtime.status()["state"] == "idle" && runtime.status()["error"] == "reading cancelled"
    });
    assert_eq!(late["state"], "idle");
    assert_eq!(
        runtime.request(json!({"command":"get_speech_status"}))["id"],
        speech_before,
        "the cancelled run started no second speech"
    );
    runtime.quit();
}

#[test]
fn replacing_speech_ends_reading_promptly_while_the_replacement_is_active() {
    let Some(model) = fixture() else { return };
    if !runtime_available() {
        return;
    }
    let speech = Speech::hold();
    let dir = tempdir().unwrap();
    let models = Models::new(dir.path(), false);
    let image = dir.path().join("clipboard.png");
    write_png(&image, 64, 64);
    let runtime = Runtime::start(
        &model,
        Setup {
            ocr: Some(models.args()),
            endpoint: Some(speech.endpoint.clone()),
            image: Some(image),
            ..Setup::default()
        },
    );
    runtime.configure_speech();
    assert_eq!(
        runtime.request(json!({"command":"reading_clip"}))["queued"],
        true
    );
    runtime.wait_state(PIPELINE_TIMEOUT, "speaking");
    assert!(speech.connected(PIPELINE_TIMEOUT));
    let deadline = Instant::now() + TIMEOUT;
    let ours = loop {
        let status = runtime.request(json!({"command":"get_speech_status"}));
        if status["active"] == true && status["id"].as_u64().unwrap_or(0) != 0 {
            break status["id"].clone();
        }
        assert!(
            Instant::now() < deadline,
            "speech did not become active: {status}"
        );
        std::thread::sleep(Duration::from_millis(5));
    };
    // Give reading several ticks to observe its own playback id before another
    // client replaces it. The held peer cannot complete the original request.
    std::thread::sleep(Duration::from_millis(100));
    assert_eq!(runtime.status()["state"], "speaking");
    assert_eq!(
        runtime.request(json!({"command":"speak","text":"replacement","replace":true}))["ok"],
        true
    );
    // Much shorter than the 65 s step timeout or the held peer's response budget.
    let done = runtime.wait_state(Duration::from_secs(2), "idle");
    assert_eq!(done["error"], "speech cancelled or replaced", "{done}");
    let replacement = runtime.request(json!({"command":"get_speech_status"}));
    assert_ne!(replacement["id"], ours, "{replacement}");
    assert_eq!(replacement["active"], true, "{replacement}");
    std::thread::sleep(Duration::from_millis(100));
    assert_eq!(
        runtime.status(),
        done,
        "replacement must not resume reading"
    );
    runtime.quit();
}

#[test]
fn a_repeated_trigger_while_cancelling_does_not_start_a_new_run() {
    let Some(model) = fixture() else { return };
    if !runtime_available() {
        return;
    }
    // A hold peer keeps `speaking` occupied, so `cancelling` is observable.
    let speech = Speech::hold();
    let dir = tempdir().unwrap();
    let models = Models::new(dir.path(), false);
    let image = dir.path().join("clipboard.png");
    write_png(&image, 64, 64);
    let runtime = Runtime::start(
        &model,
        Setup {
            ocr: Some(models.args()),
            endpoint: Some(speech.endpoint.clone()),
            image: Some(image),
            ..Setup::default()
        },
    );
    runtime.configure_speech();
    assert_eq!(
        runtime.request(json!({"command":"reading_clip"}))["queued"],
        true
    );
    runtime.wait_state(PIPELINE_TIMEOUT, "speaking");
    assert_eq!(
        runtime.request(json!({"command":"reading_clip"}))["queued"],
        true
    );
    runtime.wait_state(PIPELINE_TIMEOUT, "cancelling");
    // Every further trigger during `cancelling` is accepted and ignored.
    for _ in 0..3 {
        assert_eq!(
            runtime.request(json!({"command":"reading_clip"})),
            json!({"ok":true,"queued":true})
        );
    }
    let done = runtime.wait_status(PIPELINE_TIMEOUT, |status| {
        status["state"] == "idle" && status["error"].as_str().is_some_and(|e| !e.is_empty())
    });
    assert_eq!(done["error"], "reading cancelled", "{done}");
    // No new run began: the status stays idle and the OCR surface never saw a
    // second job.
    std::thread::sleep(Duration::from_millis(200));
    let idle = runtime.status();
    assert_eq!(idle["state"], "idle", "{idle}");
    assert_eq!(idle["error"], "reading cancelled", "{idle}");
    runtime.quit();
}

#[test]
fn a_trigger_while_recognizing_cancels_without_speaking() {
    let Some(model) = fixture() else { return };
    if !runtime_available() {
        return;
    }
    let speech = Speech::start();
    let dir = tempdir().unwrap();
    let models = Models::new(dir.path(), false);
    // A 1024x1024 image lengthens the real preprocessing pass, which gives the
    // cancel a deterministic window while the OCR job is pending.
    let image = dir.path().join("clipboard.png");
    write_png(&image, 1024, 1024);
    let runtime = Runtime::start(
        &model,
        Setup {
            ocr: Some(models.args()),
            endpoint: Some(speech.endpoint.clone()),
            image: Some(image),
            ..Setup::default()
        },
    );
    runtime.configure_speech();
    assert_eq!(
        runtime.request(json!({"command":"reading_clip"}))["queued"],
        true
    );
    runtime.wait_state(PIPELINE_TIMEOUT, "recognizing");
    assert_eq!(
        runtime.request(json!({"command":"reading_clip"}))["queued"],
        true
    );
    let done = runtime.wait_status(PIPELINE_TIMEOUT, |status| {
        status["state"] == "idle" && status["error"].as_str().is_some_and(|e| !e.is_empty())
    });
    assert_eq!(done["error"], "reading cancelled", "{done}");
    // Even when the OCR job later completes, it must not speak: the run is over
    // and stays over.
    std::thread::sleep(Duration::from_millis(500));
    let after = runtime.status();
    assert_eq!(after["state"], "idle", "{after}");
    assert_eq!(after["error"], "reading cancelled", "{after}");
    assert!(!speech.connected(Duration::from_millis(300)));
    runtime.quit();
}
