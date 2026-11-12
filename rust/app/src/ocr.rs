//! OCR command surface: `get_ocr_status`, `ocr_recognize`, `get_ocr_result` and
//! `cancel_ocr`, backed by the `deskwaifu-ocr-engine` crate.
//!
//! The replies mirror the C++ `OcrClient`/`OcrProcessor` state machine
//! (`plugins/perception/private/processor/ocr`): the status object, the
//! `{ok,job_id,state}` acceptance, the bounded "only the most recent job is
//! retained" receipt, and the terminal `response` with the native
//! `{ok,result,protocol,id}` shape. `get_ocr_result` additionally carries the
//! `text` that `OcrProcessor` derives from `result.lines`, and a `cancelled`
//! flag, because this module serves those commands directly instead of routing
//! an event to `reading_clip`.
//!
//! Threading: the engine and its ONNX sessions are created, used and dropped only
//! on the one worker thread this module owns, so loading two sessions never
//! stalls the IPC accept loop. The loop hands over at most one request (a second
//! submit is refused with "OCR is busy") and drains one reply channel on its
//! existing 16 ms tick. There is exactly one thread per process, started by the
//! first accepted job and joined when this module drops.
//!
//! Cancellation is **not** cooperative: `deskwaifu-ocr-engine` exposes no cancel
//! hook, and interrupting ONNX Runtime mid-inference is not something this crate
//! can do. `cancel_ocr` therefore only changes the host-side state: the job is
//! marked cancelled, its result is discarded when the worker eventually delivers
//! it, and a flag lets the worker skip work for a job cancelled before it starts.
//! The status keeps reporting `busy` until that late completion arrives, exactly
//! like the C++ client did while its work thread drained.

use deskwaifu_config::settings::ConfigDocument;
use deskwaifu_ocr_engine::{Engine, Error, Recognition};
use serde_json::{json, Value};
use std::env;
use std::fs;
use std::io::ErrorKind;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver, Sender, TryRecvError};
use std::sync::Arc;
use std::thread::JoinHandle;

/// The C++ `Submit` rejects every unusable path with this one sentence.
const IMAGE_ERROR: &str = "image must be an absolute readable file path";
/// The C++ `Submit` refuses overlapping work with this text.
const BUSY_ERROR: &str = "OCR is busy";
/// The C++ `Result` reports an unknown or evicted job with this text.
const UNKNOWN_JOB: &str = "unknown OCR job; only the most recent job is retained";
/// The C++ `Cancel` reports a non-pending job with this text.
const NOTHING_CANCELLED: &str = "no matching pending OCR job";
/// The C++ `Fail("OCR request cancelled", "cancelled")` text.
const CANCELLED_ERROR: &str = "OCR request cancelled";
/// Reported when a resolved model directory is not on disk. OCR stays disabled
/// rather than failing startup; the text names the path and the setting that
/// produced it.
const MISSING_DIRECTORY: &str = "OCR is not available; model directory does not exist";
/// The two system directories `OcrClient::ConfigureModels` starts from.
const DEFAULT_DETECTION: &str = "/usr/share/deskwaifu/ocr/models/PP-OCRv6_small_det";
const DEFAULT_RECOGNITION: &str = "/usr/share/deskwaifu/ocr/models/PP-OCRv6_small_rec";
/// The keys the C++ validates, in its order.
const DETECTION_KEY: &str = "ocr_detection_model_dir";
const RECOGNITION_KEY: &str = "ocr_recognition_model_dir";

/// The two model directories, matching the C++ configuration keys
/// `ocr_detection_model_dir` and `ocr_recognition_model_dir`.
#[derive(Clone, Debug)]
pub struct Config {
    pub detection: PathBuf,
    pub recognition: PathBuf,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            detection: PathBuf::from(DEFAULT_DETECTION),
            recognition: PathBuf::from(DEFAULT_RECOGNITION),
        }
    }
}

/// `<configRoot>/plugin/perception/config.json`, the file the C++ `OcrProcessor`
/// reads through its `PluginContext`. `configRoot` is the directory holding the
/// main `deskwaifu/config.json`, i.e. `QStandardPaths::ConfigLocation` plus
/// `deskwaifu`, so the whole path is
/// `$XDG_CONFIG_HOME/deskwaifu/plugin/perception/config.json`, falling back to
/// `$HOME/.config/deskwaifu/plugin/perception/config.json`.
pub fn config_path() -> PathBuf {
    let root = env::var_os("XDG_CONFIG_HOME")
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
        .or_else(|| env::var_os("HOME").map(|home| PathBuf::from(home).join(".config")))
        .unwrap_or_else(|| PathBuf::from(".config"));
    // The same `<configRoot>/plugin/<plugin>/config.json` layout the C++
    // `PluginContext` gives every plugin, and that the speech key file uses.
    root.join("deskwaifu")
        .join("plugin")
        .join("perception")
        .join("config.json")
}

/// Resolve both model directories. Explicit arguments win outright; otherwise
/// the perception plugin configuration supplies them, a missing key keeps the
/// system default for that directory and a missing file leaves both defaults in
/// place. A present value that is not a usable absolute directory is a hard
/// failure, so no broken setting silently turns into a system path.
pub fn config(explicit: Option<Config>) -> Result<Config, String> {
    if let Some(config) = explicit {
        validate(&config.detection, "--ocr-detection-dir")?;
        validate(&config.recognition, "--ocr-recognition-dir")?;
        return Ok(config);
    }
    let path = config_path();
    let bytes = match fs::read(&path) {
        Ok(bytes) => bytes,
        Err(error) if error.kind() == ErrorKind::NotFound => return Ok(Config::default()),
        Err(error) => {
            return Err(format!(
                "cannot read OCR configuration {}: {error}",
                path.display()
            ))
        }
    };
    let text = String::from_utf8(bytes)
        .map_err(|_| format!("invalid OCR configuration {}: not UTF-8", path.display()))?;
    // The C++ `ReadJsonObject` parses with `QJsonDocument`, which rejects comments.
    let document = ConfigDocument::parse(&text, false)
        .map_err(|error| format!("invalid OCR configuration {}: {error}", path.display()))?;
    Ok(Config {
        detection: directory(&document, DETECTION_KEY, DEFAULT_DETECTION)?,
        recognition: directory(&document, RECOGNITION_KEY, DEFAULT_RECOGNITION)?,
    })
}

/// One configured directory. This is the C++ `ConfigureModels` validation: a
/// present value must be a string without a NUL character and an absolute path.
/// `~` and environment variables are not expanded, and nothing falls back.
fn directory(document: &ConfigDocument, key: &str, fallback: &str) -> Result<PathBuf, String> {
    let Some(raw) = document.values.get(key) else {
        return Ok(PathBuf::from(fallback));
    };
    let value = serde_json::from_str::<String>(raw)
        .map_err(|_| format!("OCR configuration requires an absolute directory: {key}"))?;
    if value.contains('\0') {
        return Err(format!(
            "OCR configuration requires an absolute directory: {key}"
        ));
    }
    validate(Path::new(&value), key)
}

/// The same shape check applied to the command-line arguments, which cannot
/// carry a NUL byte but can still be empty or relative.
fn validate(path: &Path, source: &str) -> Result<PathBuf, String> {
    if path.as_os_str().is_empty() || !path.is_absolute() {
        return Err(format!(
            "OCR configuration requires an absolute directory: {source}"
        ));
    }
    Ok(path.to_owned())
}

/// One unit of work for the worker thread.
struct Request {
    id: String,
    image: PathBuf,
    /// Set by `cancel_ocr`; the worker checks it before it starts inferring.
    cancelled: Arc<AtomicBool>,
}

/// What the worker reports back to the loop.
enum Reply {
    /// The result of the one-time model load, so `get_ocr_status` can report a
    /// load failure even before the failed job's own reply is read.
    Loaded(Result<(), Failure>),
    Finished {
        id: String,
        result: Result<Recognition, Failure>,
    },
}

/// A failure carried back as text plus the engine's stable error code.
#[derive(Clone)]
struct Failure {
    code: &'static str,
    message: String,
}

impl Failure {
    fn of(error: Error) -> Self {
        Self {
            code: error.code(),
            message: error.to_string(),
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Availability {
    /// Directories are configured but nothing was loaded yet (`unchecked`).
    Unchecked,
    /// The engine loaded once; later failures are per-job, like the C++.
    Available,
    /// The model load failed (`unavailable`).
    Unavailable,
}

impl Availability {
    fn name(self) -> &'static str {
        match self {
            Availability::Unchecked => "unchecked",
            Availability::Available => "available",
            Availability::Unavailable => "unavailable",
        }
    }
}

/// The C++ `_state` values. There is no separate cancelled state: `Fail` turns a
/// pending job into `failed`, and [`Job::cancelled`] records why.
#[derive(Clone, Copy, PartialEq, Eq)]
enum State {
    Idle,
    Pending,
    Completed,
    Failed,
}

impl State {
    fn name(self) -> &'static str {
        match self {
            State::Idle => "idle",
            State::Pending => "pending",
            State::Completed => "completed",
            State::Failed => "failed",
        }
    }
}

/// The most recent job. A newer accepted job replaces it outright.
struct Job {
    id: String,
    state: State,
    cancelled: bool,
    /// The terminal `response` object, in the native module's shape.
    response: Option<Value>,
    /// `OcrProcessor`'s newline-joined `text` for a completed job.
    text: Option<String>,
    /// Shared with the worker so a cancel can skip work that has not started.
    cancel: Arc<AtomicBool>,
}

struct Worker {
    /// `None` once dropped, which is what ends the worker loop.
    requests: Option<Sender<Request>>,
    replies: Receiver<Reply>,
    thread: Option<JoinHandle<()>>,
}

impl Drop for Worker {
    fn drop(&mut self) {
        // Closing the request channel makes `serve` return after whatever
        // inference is already running, so the engine and its ONNX sessions are
        // dropped on their own thread rather than mid-call here.
        self.requests = None;
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

pub struct Ocr {
    config: Config,
    worker: Option<Worker>,
    job: Option<Job>,
    /// Id of the request the worker has not reported back on yet. It keeps
    /// `busy` true while a cancelled job drains, and no second request is sent.
    outstanding: Option<String>,
    availability: Availability,
    loaded: bool,
    error: Option<Failure>,
    jobs: u64,
}

impl Ocr {
    /// A resolved directory that is not on disk leaves OCR unavailable with the
    /// offending paths in its error instead of failing startup: a missing model
    /// directory is a deployment gap, and `get_ocr_status` is how a client learns
    /// about it. Only configuration *shape* errors are fatal, and those are
    /// rejected before this point.
    pub fn new(config: Config) -> Self {
        let missing = [
            (&config.detection, DETECTION_KEY),
            (&config.recognition, RECOGNITION_KEY),
        ]
        .into_iter()
        .filter(|(path, _)| !path.is_dir())
        .map(|(path, key)| format!("{} ({key})", path.display()))
        .collect::<Vec<_>>();
        let error = (!missing.is_empty()).then(|| Failure {
            code: "model_missing",
            message: format!(
                "{MISSING_DIRECTORY}: {}; set an existing directory in the perception \
                 configuration or pass --ocr-detection-dir and --ocr-recognition-dir",
                missing.join(", ")
            ),
        });
        Self {
            config,
            worker: None,
            job: None,
            outstanding: None,
            availability: if error.is_some() {
                Availability::Unavailable
            } else {
                Availability::Unchecked
            },
            loaded: false,
            error,
            jobs: 0,
        }
    }

    /// The C++ `OcrClient::Status` object, plus an explicit `loaded` flag; its
    /// `running` only said "loaded or working". `retiring` is always false: this
    /// runtime never releases the engine to reclaim an idle worker.
    pub fn status(&self) -> Value {
        json!({
            "ok": true,
            "available": self.availability == Availability::Available,
            "availability_state": self.availability.name(),
            "error_code": self.error.as_ref().map_or("", |failure| failure.code),
            "error": self.error.as_ref().map_or("", |failure| failure.message.as_str()),
            "detection_model_dir": self.config.detection.display().to_string(),
            "recognition_model_dir": self.config.recognition.display().to_string(),
            "state": self.job.as_ref().map_or(State::Idle, |job| job.state).name(),
            "job_id": self.job.as_ref().map_or("", |job| job.id.as_str()),
            "loaded": self.loaded,
            "running": self.loaded || self.outstanding.is_some(),
            "busy": self.outstanding.is_some(),
            "retiring": false,
            "execution_mode": "in_process",
        })
    }

    /// `ocr_recognize`: validate, then accept one job and answer `job_id` at
    /// once. The order follows `OcrClient::Submit`: busy, availability, image.
    pub fn submit(&mut self, args: &Value) -> Result<Value, String> {
        if self.outstanding.is_some() {
            return Err(BUSY_ERROR.into());
        }
        if self.availability != Availability::Available {
            if let Some(failure) = &self.error {
                return Err(failure.message.clone());
            }
        }
        let image = image_path(args)?;
        let config = self.config.clone();

        self.jobs += 1;
        let id = format!("{}-{}", std::process::id(), self.jobs);
        let cancel = Arc::new(AtomicBool::new(false));
        self.outstanding = Some(id.clone());
        self.job = Some(Job {
            id: id.clone(),
            state: State::Pending,
            cancelled: false,
            response: None,
            text: None,
            cancel: cancel.clone(),
        });
        let request = Request {
            id: id.clone(),
            image,
            cancelled: cancel,
        };
        if self.start(&config)?.send(request).is_err() {
            // The worker thread died. Drop it so the next accepted job starts a
            // fresh one, and do not leave the slot claiming to be busy.
            self.worker = None;
            self.outstanding = None;
            self.job = None;
            let failure = Failure {
                code: "backend_error",
                message: "OCR worker stopped".to_string(),
            };
            self.availability = Availability::Unavailable;
            self.error = Some(failure.clone());
            return Err(failure.message);
        }
        Ok(json!({"ok": true, "job_id": id, "state": "pending"}))
    }

    /// `get_ocr_result`: a pending job reports pending, a terminal one reports
    /// the native response, and anything but the retained job fails.
    pub fn result(&self, args: &Value) -> Result<Value, String> {
        let id = job_id(args);
        let job = match &self.job {
            Some(job) if !id.is_empty() && job.id == id => job,
            _ => return Err(UNKNOWN_JOB.into()),
        };
        let mut reply = json!({"ok": true, "job_id": job.id, "state": job.state.name()});
        if job.state == State::Pending {
            return Ok(reply);
        }
        reply["cancelled"] = json!(job.cancelled);
        reply["response"] = job.response.clone().unwrap_or(Value::Null);
        if let Some(text) = &job.text {
            reply["text"] = json!(text);
        }
        Ok(reply)
    }

    /// `cancel_ocr`: only the matching pending job is cancelled. The engine keeps
    /// running; its eventual result is dropped by [`Ocr::apply`].
    pub fn cancel(&mut self, args: &Value) -> Result<Value, String> {
        let id = job_id(args);
        let job = match &mut self.job {
            Some(job) if !id.is_empty() && job.id == id && job.state == State::Pending => job,
            _ => return Err(NOTHING_CANCELLED.into()),
        };
        job.state = State::Failed;
        job.cancelled = true;
        // The C++ `Fail` stores a plain error object; only a backend reply gets
        // `protocol` and `id` added.
        job.response = Some(json!({"ok": false, "error": CANCELLED_ERROR}));
        job.cancel.store(true, Ordering::SeqCst);
        Ok(json!({"ok": true, "job_id": job.id}))
    }

    /// Drain worker replies. Called once per 16 ms tick of the runtime loop.
    pub fn poll(&mut self) {
        let mut replies = Vec::new();
        let mut stopped = false;
        if let Some(worker) = self.worker.as_ref() {
            loop {
                match worker.replies.try_recv() {
                    Ok(reply) => replies.push(reply),
                    Err(TryRecvError::Empty) => break,
                    // The worker thread is gone, so no reply is coming for the
                    // outstanding slot. Without this the job would report busy
                    // forever.
                    Err(TryRecvError::Disconnected) => {
                        stopped = true;
                        break;
                    }
                }
            }
        }
        for reply in replies {
            self.apply(reply);
        }
        if stopped && self.outstanding.is_some() {
            self.outstanding = None;
            let failure = Failure {
                code: "backend_error",
                message: "OCR worker stopped".to_string(),
            };
            self.availability = Availability::Unavailable;
            self.error = Some(failure.clone());
            if let Some(job) = self.job.as_mut() {
                if job.state == State::Pending {
                    job.state = State::Failed;
                    job.response = Some(json!({"ok": false, "error": failure.message}));
                }
            }
        }
    }

    fn start(&mut self, config: &Config) -> Result<&Sender<Request>, String> {
        if self.worker.is_none() {
            self.worker = Some(Worker::start(config.clone()));
        }
        self.worker
            .as_ref()
            .and_then(|worker| worker.requests.as_ref())
            .ok_or_else(|| "OCR worker stopped".to_string())
    }

    fn apply(&mut self, reply: Reply) {
        match reply {
            Reply::Loaded(Ok(())) => {
                self.availability = Availability::Available;
                self.loaded = true;
                self.error = None;
            }
            Reply::Loaded(Err(failure)) => {
                self.availability = Availability::Unavailable;
                self.error = Some(failure);
            }
            Reply::Finished { id, result } => {
                if self.outstanding.as_deref() == Some(id.as_str()) {
                    self.outstanding = None;
                }
                let job = match self.job.as_mut() {
                    // A cancelled or replaced job's late completion is discarded
                    // here and never becomes text for a later `get_ocr_result`.
                    Some(job) if job.id == id && job.state == State::Pending => job,
                    _ => return,
                };
                match result {
                    Ok(recognition) => {
                        job.state = State::Completed;
                        job.text = Some(join_lines(&recognition));
                        job.response = Some(json!({
                            "ok": true,
                            "result": result_value(&recognition),
                            "protocol": 1,
                            "id": job.id,
                        }));
                    }
                    Err(failure) => {
                        job.state = State::Failed;
                        job.response = Some(json!({
                            "ok": false,
                            "error": failure.message,
                            "protocol": 1,
                            "id": job.id,
                        }));
                    }
                }
            }
        }
    }
}

impl Worker {
    fn start(config: Config) -> Self {
        let (requests, incoming) = mpsc::channel();
        let (outgoing, replies) = mpsc::channel();
        let thread = std::thread::spawn(move || serve(&config, &incoming, &outgoing));
        Self {
            requests: Some(requests),
            replies,
            thread: Some(thread),
        }
    }
}

/// The worker body: load the engine once, then run one request at a time. Every
/// accepted request produces exactly one [`Reply::Finished`], so the loop never
/// waits on a slot that will not be released.
fn serve(config: &Config, incoming: &Receiver<Request>, replies: &Sender<Reply>) {
    let mut engine: Option<Engine> = None;
    while let Ok(request) = incoming.recv() {
        if engine.is_none() {
            match Engine::load(&config.detection, &config.recognition) {
                Ok(loaded) => {
                    engine = Some(loaded);
                    let _ = replies.send(Reply::Loaded(Ok(())));
                }
                Err(error) => {
                    let failure = Failure::of(error);
                    let _ = replies.send(Reply::Loaded(Err(failure.clone())));
                    // The failed job reports the same load error through its own
                    // terminal reply; the model is not retried, matching the C++
                    // client's "no background retry" behavior.
                    let _ = replies.send(Reply::Finished {
                        id: request.id,
                        result: Err(failure),
                    });
                    continue;
                }
            }
        }
        // The only real cancellation this crate can offer: skip a job the host
        // gave up on before the expensive pass starts.
        let result = if request.cancelled.load(Ordering::SeqCst) {
            Err(Failure {
                code: "cancelled",
                message: CANCELLED_ERROR.to_string(),
            })
        } else {
            match engine.as_ref() {
                Some(engine) => engine.recognize_file(&request.image).map_err(Failure::of),
                None => Err(Failure {
                    code: "backend_error",
                    message: "OCR engine is not loaded".to_string(),
                }),
            }
        };
        let _ = replies.send(Reply::Finished {
            id: request.id,
            result,
        });
    }
}

/// `image` must be a string naming an absolute, readable regular file. Every
/// failure reports the C++ text, so a client cannot tell a missing file from a
/// directory or a relative path — the C++ `Submit` does not either.
fn image_path(args: &Value) -> Result<PathBuf, String> {
    let text = match args.get("image") {
        Some(Value::String(text)) => text,
        _ => return Err(IMAGE_ERROR.into()),
    };
    let path = Path::new(text);
    if !path.is_absolute() {
        return Err(IMAGE_ERROR.into());
    }
    let metadata = std::fs::metadata(path).map_err(|_| IMAGE_ERROR.to_string())?;
    if !metadata.is_file() {
        return Err(IMAGE_ERROR.into());
    }
    std::fs::File::open(path).map_err(|_| IMAGE_ERROR.to_string())?;
    Ok(path.to_owned())
}

/// `job_id` as the C++ `request.value("job_id").toString()` reads it: a missing
/// or non-string field is empty and therefore matches no job.
fn job_id(args: &Value) -> &str {
    args.get("job_id")
        .and_then(Value::as_str)
        .unwrap_or_default()
}

/// `OcrProcessor` joins the recognized line texts with newlines.
fn join_lines(recognition: &Recognition) -> String {
    recognition
        .lines
        .iter()
        .map(|line| line.text.as_str())
        .collect::<Vec<_>>()
        .join("\n")
}

/// The native `result` object, in the key order `Recognition::to_json` writes.
fn result_value(recognition: &Recognition) -> Value {
    serde_json::from_str(&recognition.to_json()).unwrap_or(Value::Null)
}
