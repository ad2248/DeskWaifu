//! Clipboard reading-aloud pipeline: `reading_clip` and
//! `get_reading_clip_status`.
//!
//! This is the command-level port of `plugins/reading_clip/private/processor/
//! reading_clip/ReadingClipProcessor.cpp`: a clipboard image is read once, run
//! through OCR, and the recognized text is spoken. The state machine, the state
//! names and the error texts come from that processor; the steps themselves are
//! [`crate::ocr`] and [`crate::speech`], which this module drives instead of
//! routing capability events.
//!
//! # Replies
//!
//! `reading_clip` answers the C++ `{ok:true,queued:true}` at once. It only means
//! the trigger was accepted: the run starts on the next 16 ms tick, which is the
//! event the C++ posts. `get_reading_clip_status` answers the C++
//! `{ok,state,error}`, where `error` is `""` when no run has failed.
//!
//! # States
//!
//! `idle → reading_image → recognizing → speaking → idle`. The C++ `Status()`
//! does expose its internal `cancelling` state (it is one of the five names in
//! the processor's status array), so this module does too: a trigger while busy
//! answers `queued` and moves the run to `cancelling`, and the run then ends in
//! `idle` with `reading cancelled`. Repeated triggers while `cancelling` change
//! nothing and start no new run.
//!
//! Every transition is applied on a later tick than the step that caused it, so
//! each intermediate state stays observable. Completion requires the speech to
//! actually finish (its state machine reaching `idle` with `active:false`), not
//! for the speak request to be accepted.
//!
//! # Clipboard access
//!
//! Wayland only: `wl-paste --no-newline --type image/png` (wl-clipboard is a
//! packaged runtime dependency) runs in a worker thread with a bounded timeout,
//! writing the bytes straight to a temporary PNG that lives until the OCR job is
//! done (IM-02). Its dimensions are checked from the PNG header before OCR, so
//! an image wider or taller than 4096 is rejected without inference.
//!
//! `DESKWAIFU_READING_TEST_IMAGE=<png path>` replaces the clipboard read with
//! that file's bytes. It is a **test-only** seam (the C++ had a clipboard
//! injection point of the same kind) and is never read in production paths
//! other than here.
//!
//! # Divergences from the C++
//!
//! - The C++ read through `QClipboard` and encoded a `QImage`; this runtime has
//!   no Qt, so the clipboard is `wl-paste` output and every error text that the
//!   C++ produced for "no image" or "too large" is reused where it still means
//!   the same thing.
//! - The C++ cancelled a speech step through `speech.speak/v1`'s cancel event.
//!   Speech here has no per-request cancel command, so cancelling during
//!   `speaking` only stops the run from following that playback; the run ends
//!   once that playback is no longer active. The audio itself is not interrupted.

use crate::ocr::Ocr;
use crate::speech::Speech;
use serde_json::{json, Value};
use std::env;
use std::fs;
use std::io::ErrorKind;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::mpsc::{self, Receiver};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

/// The C++ `ImageProcessor` text for a clipboard without an image; also used
/// for empty `wl-paste` output, which means the same thing here.
const NO_IMAGE: &str = "clipboard contains no image";
/// The C++ `ImageProcessor` text for an oversized image or a failed PNG encode.
const IMAGE_FAILED: &str = "image is too large or PNG encoding failed";
/// The C++ `ReadingClipProcessor::Result` text for an empty recognition.
const NO_TEXT: &str = "no text recognized";
/// The C++ `ReadingClipProcessor` text once a cancelled run drains.
const CANCELLED: &str = "reading cancelled";
/// The C++ `SpeechProcessor` text when a playback ends without completing.
const SPEECH_REPLACED: &str = "speech cancelled or replaced";
/// Reported when `wl-paste` is not installed.
const WL_PASTE_MISSING: &str =
    "wl-paste is not available; install wl-clipboard to read clipboard images";
/// Reported when `wl-paste` does not answer in time.
const WL_PASTE_TIMEOUT: &str = "wl-paste did not answer within 5 s";
/// Reported when OCR returns a failed job without its own text.
const OCR_FAILED: &str = "OCR job failed";
/// The test-only clipboard replacement.
const TEST_IMAGE: &str = "DESKWAIFU_READING_TEST_IMAGE";
/// The C++ `CreateImageFile` bound, and the native engine's own limit.
const MAX_SIDE: u32 = 4096;
/// How long one clipboard read may take before the worker is killed.
const READ_TIMEOUT: Duration = Duration::from_secs(5);
/// How often the worker polls the `wl-paste` child.
const READ_POLL: Duration = Duration::from_millis(10);
/// The C++ `Request(..., 65000)` budget for one dynamic capability step.
const STEP_TIMEOUT: Duration = Duration::from_secs(65);
/// The C++ `Finish("reading capability: " + RequestStatusName(TimedOut))` text.
const TIMED_OUT: &str = "reading capability: timed out";
/// How many 16 ms ticks `cancelling` stays visible. The C++ state was observable
/// until the cancelled step's cancel drained; a step that needs no draining gets
/// this small dwell instead, so the state can still be polled.
const CANCEL_TICKS: u32 = 4;

/// The C++ `ReadingClipProcessor::State`, including the internal `cancelling`
/// state that `Status()` exposes.
#[derive(Clone, Copy, PartialEq, Eq)]
enum State {
    Idle,
    ReadingImage,
    Recognizing,
    Speaking,
    Cancelling,
}

impl State {
    fn name(self) -> &'static str {
        match self {
            State::Idle => "idle",
            State::ReadingImage => "reading_image",
            State::Recognizing => "recognizing",
            State::Speaking => "speaking",
            State::Cancelling => "cancelling",
        }
    }
}

/// What the current state is waiting for. Kept out of [`State`] because the
/// C++ state names are the externally visible contract and this is bookkeeping.
enum Wait {
    None,
    /// The clipboard worker is running.
    Read,
    /// The OCR job this run submitted.
    Ocr(String),
    /// The speech playback this run started.
    Speak {
        token: u64,
    },
}

/// The clipboard reading pipeline. It owns only host-side state: the clipboard
/// worker, the temporary PNG and the OCR/speech steps it is waiting for. All
/// progress is drained on the runtime's 16 ms tick.
pub struct Reading {
    state: State,
    error: String,
    /// Bumped by every trigger and every cancel. A step belongs to the run that
    /// started it, so a late completion from an earlier run is dropped.
    run: u64,
    queued: bool,
    reader: Option<Reader>,
    /// The temporary PNG; dropped as soon as the OCR step ends (IM-02).
    image: Option<TempImage>,
    /// The recognized text, between the OCR and speech steps.
    text: Option<String>,
    wait: Wait,
    /// Ticks the current state has been visible for. A step never advances on the
    /// very first tick after its state was entered, so every state in the C++
    /// sequence can be read back through `get_reading_clip_status` instead of
    /// being skipped inside one 16 ms tick.
    ticks: u32,
    /// RC-03: every step has the C++ 65000 ms budget.
    deadline: Option<Instant>,
    /// Set once the OCR job was asked to cancel, so it is asked only once.
    ocr_cancelled: bool,
}

impl Default for Reading {
    fn default() -> Self {
        Self::new()
    }
}

impl Reading {
    pub fn new() -> Self {
        Self {
            state: State::Idle,
            error: String::new(),
            run: 0,
            queued: false,
            reader: None,
            image: None,
            text: None,
            wait: Wait::None,
            ticks: 0,
            deadline: None,
            ocr_cancelled: false,
        }
    }

    /// `get_reading_clip_status`: `{ok,state,error}`, exactly the C++
    /// `ReadingClipProcessor::Status()`.
    pub fn status(&self) -> Value {
        json!({"ok": true, "state": self.state.name(), "error": self.error})
    }

    /// `reading_clip`: accept the trigger, answer `{ok:true,queued:true}` and
    /// let a later tick start the run. A trigger while `cancelling` is still
    /// answered `queued` but starts nothing, like the C++ event that arrives
    /// during cancellation.
    pub fn trigger(&mut self) -> Value {
        if self.state == State::Idle {
            self.queued = true;
        } else if self.state != State::Cancelling {
            // `cancelling` is a real state for at least one tick, like the C++
            // state the processor reports before the drained step finishes.
            self.state = State::Cancelling;
            self.ticks = 0;
            if self.deadline.is_none() {
                self.deadline = Some(Instant::now() + STEP_TIMEOUT);
            }
        }
        json!({"ok": true, "queued": true})
    }

    /// One tick of the pipeline. Called from the runtime loop next to `poll` of
    /// the OCR and speech surfaces, after they drained their own workers.
    pub fn poll(&mut self, ocr: &mut Ocr, speech: &mut Speech) {
        if self.state == State::Idle {
            if self.queued {
                self.queued = false;
                self.begin();
            }
            return;
        }
        self.ticks = self.ticks.saturating_add(1);
        // A state is entered during a tick; this gate gives a client at least one
        // whole tick to observe it before the step can advance past it.
        if self.ticks <= 1 {
            return;
        }
        if self.state == State::Cancelling && self.expired() {
            return self.fail(CANCELLED);
        }
        match self.state {
            State::Idle => {}
            State::ReadingImage => self.image_step(ocr),
            State::Recognizing => self.ocr_step(ocr),
            State::Speaking => self.speech_step(speech),
            State::Cancelling => self.cancel_step(ocr),
        }
    }

    /// Starts a fresh run: clears the previous error and asks for the clipboard.
    fn begin(&mut self) {
        self.run += 1;
        self.error.clear();
        self.text = None;
        self.reader = Some(Reader::start(self.run));
        self.enter(State::ReadingImage, Wait::Read);
    }

    /// Enters `state` waiting for `wait`, starting that step's budget.
    fn enter(&mut self, state: State, wait: Wait) {
        self.state = state;
        self.wait = wait;
        self.ticks = 0;
        self.deadline = Some(Instant::now() + STEP_TIMEOUT);
    }

    /// Ends the run, keeping `error` (empty on success). Every resource the run
    /// held is released here.
    fn fail(&mut self, error: &str) {
        self.error = error.to_owned();
        self.reset();
    }

    fn reset(&mut self) {
        self.state = State::Idle;
        self.wait = Wait::None;
        self.ticks = 0;
        self.deadline = None;
        self.image = None;
        self.text = None;
        self.reader = None;
        self.ocr_cancelled = false;
    }

    /// Whether the current step ran past its budget.
    fn expired(&self) -> bool {
        self.deadline
            .is_some_and(|deadline| Instant::now() >= deadline)
    }

    /// `reading_image`: take the clipboard worker's result once it reported, then
    /// start the OCR job on a later tick so `recognizing` is observable.
    fn image_step(&mut self, ocr: &mut Ocr) {
        if self.expired() {
            return self.fail(TIMED_OUT);
        }
        let Some(reader) = self.reader.as_mut() else {
            return self.fail(IMAGE_FAILED);
        };
        if !reader.done() {
            return;
        }
        let (run, result) = self.reader.as_mut().unwrap().take();
        self.reader = None;
        // A read that answered after its run was cancelled (RC-03) must not
        // start an OCR job, and its temporary file is dropped here.
        if run != self.run || self.state != State::ReadingImage {
            return self.fail(IMAGE_FAILED);
        }
        match result {
            Ok(image) => {
                let path = image.path.clone();
                self.image = Some(image);
                match ocr.submit(&json!({"image": path})) {
                    Ok(reply) => {
                        let id = reply["job_id"].as_str().unwrap_or_default().to_owned();
                        self.enter(State::Recognizing, Wait::Ocr(id));
                    }
                    Err(error) => self.fail(&error),
                }
            }
            Err(error) => self.fail(&error),
        }
    }

    /// `recognizing`: wait for the OCR job, then either fail with its text or
    /// move to `speaking`. The temporary PNG is released as soon as the job is
    /// terminal (IM-02).
    fn ocr_step(&mut self, ocr: &mut Ocr) {
        if self.expired() {
            return self.fail(TIMED_OUT);
        }
        let Wait::Ocr(id) = &self.wait else {
            return self.fail(OCR_FAILED);
        };
        let reply = match ocr.result(&json!({"job_id": id})) {
            Ok(reply) => reply,
            Err(error) => return self.fail(&error),
        };
        if reply["state"] == "pending" {
            return;
        }
        self.image = None;
        if reply["state"] != "completed" {
            let error = reply["response"]["error"]
                .as_str()
                .filter(|text| !text.is_empty())
                .unwrap_or(OCR_FAILED)
                .to_owned();
            return self.fail(&error);
        }
        let text = reply["text"].as_str().unwrap_or_default();
        if text.trim().is_empty() {
            return self.fail(NO_TEXT);
        }
        self.text = Some(text.to_owned());
        self.enter(State::Speaking, Wait::None);
    }

    /// `speaking`: submit the speak request on the tick after the state became
    /// visible, then follow that playback until the speech machine is idle.
    fn speech_step(&mut self, speech: &mut Speech) {
        if self.expired() {
            return self.fail(TIMED_OUT);
        }
        if matches!(self.wait, Wait::None) {
            let Some(text) = self.text.take() else {
                return self.fail(NO_TEXT);
            };
            match speech.speak_with_token(&json!({"text": text})) {
                Ok(token) => self.wait = Wait::Speak { token },
                Err(error) => return self.fail(&error),
            }
            return;
        }
        let Wait::Speak { token } = self.wait else {
            return self.fail(SPEECH_REPLACED);
        };
        let status = speech.status();
        let active = status["active"].as_bool().unwrap_or(false);
        let error = status["last_error"]
            .as_str()
            .filter(|text| !text.is_empty());
        if speech.current_request_token() != token {
            return self.fail(SPEECH_REPLACED);
        }
        if let Some(error) = error {
            return self.fail(error);
        }
        if !active {
            self.complete();
        }
    }

    fn complete(&mut self) {
        self.error.clear();
        self.reset();
    }

    /// `cancelling`: stop the current step and wait for it to drain, then go
    /// idle with `reading cancelled`. A repeated trigger during this state is
    /// already ignored by [`Reading::trigger`].
    fn cancel_step(&mut self, ocr: &mut Ocr) {
        // `cancelling` is the C++ state a client can poll, so it is held for a
        // few ticks even when the cancelled step needs no draining. Without a
        // dwell it could come and go inside one 16 ms tick and be unobservable.
        if self.ticks <= CANCEL_TICKS {
            return;
        }
        match std::mem::replace(&mut self.wait, Wait::None) {
            Wait::None => self.fail(CANCELLED),
            Wait::Read => {
                // The running read is detached rather than awaited: it has its
                // own bounded timeout, and joining it here would stall the tick.
                if let Some(reader) = self.reader.as_mut() {
                    reader.detach();
                }
                self.reader = None;
                self.fail(CANCELLED)
            }
            Wait::Ocr(id) => {
                if !self.ocr_cancelled {
                    self.ocr_cancelled = true;
                    // Only a matching pending job is cancelled; a job that just
                    // finished reports an error here and is drained below.
                    let _ = ocr.cancel(&json!({"job_id": id}));
                }
                if ocr.status()["busy"].as_bool().unwrap_or(false) {
                    self.wait = Wait::Ocr(id);
                    return;
                }
                self.fail(CANCELLED)
            }
            Wait::Speak { .. } => {
                // Divergence: the C++ cancelled `speech.speak/v1` and waited for
                // that cancel to land. This runtime's speech surface has no
                // per-request cancel command, so the playback cannot be stopped
                // from here. The run therefore stops following it and ends now;
                // the audio may keep playing, but its outcome and the cancel are
                // both unable to move this run (RC-03: a late completion must not
                // resume a cancelled run).
                self.fail(CANCELLED)
            }
        }
    }
}

/// A temporary PNG that deletes itself. The C++ `ImageFile` lived in a
/// `QTemporaryDir` under `image.png`; the file must outlive the OCR job that
/// borrows it, so the run drops this only once the job is terminal.
struct TempImage {
    path: String,
    directory: PathBuf,
}

impl Drop for TempImage {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.path);
        let _ = fs::remove_dir(&self.directory);
    }
}

/// The clipboard read worker. It runs `wl-paste` on its own thread so the tick
/// never waits for the compositor, and reports one result through a channel,
/// tagged with the run that asked for it.
struct Reader {
    result: Receiver<(u64, Result<TempImage, String>)>,
    /// The reported result, collected once by [`Reader::done`].
    reported: Option<(u64, Result<TempImage, String>)>,
    thread: Option<JoinHandle<()>>,
}

impl Reader {
    fn start(run: u64) -> Self {
        let (sender, result) = mpsc::channel();
        let thread = thread::spawn(move || {
            let _ = sender.send((run, read_clipboard(run)));
        });
        Self {
            result,
            reported: None,
            thread: Some(thread),
        }
    }

    /// Whether the worker already reported, collecting its result without
    /// blocking when it did.
    fn done(&mut self) -> bool {
        if self.reported.is_none() {
            self.reported = self.result.try_recv().ok();
        }
        self.reported.is_some()
    }

    /// Takes the reported result. Only called once [`Reader::done`] is true.
    fn take(&mut self) -> (u64, Result<TempImage, String>) {
        self.reported.take().unwrap_or_else(|| {
            (
                0,
                Err("clipboard reader stopped without a result".to_string()),
            )
        })
    }

    /// Gives up on the worker without waiting for it: a cancelled read is still
    /// bounded by its own timeout, and its tagged result is discarded.
    fn detach(&mut self) {
        self.thread = None;
    }
}

impl Drop for Reader {
    fn drop(&mut self) {
        // The read has its own bounded timeout; joining here would make the tick
        // wait for the compositor, so the thread is detached instead.
        self.thread = None;
    }
}

/// Reads one clipboard image, checks it and writes it to a temporary PNG.
///
/// `DESKWAIFU_READING_TEST_IMAGE` replaces the clipboard with a file's bytes; it
/// is the test seam for a Wayland clipboard a CI run does not have.
fn read_clipboard(run: u64) -> Result<TempImage, String> {
    let bytes = match env::var_os(TEST_IMAGE) {
        Some(path) => {
            let path = Path::new(&path);
            fs::read(path).map_err(|error| {
                format!(
                    "cannot read test clipboard image {}: {error}",
                    path.display()
                )
            })?
        }
        None => paste()?,
    };
    if bytes.is_empty() {
        return Err(NO_IMAGE.into());
    }
    // `CreateImageFile` rejects a null image, a side over 4096, or a failed PNG
    // encode with one text; a reader that cannot decode the header is the same
    // failure, so no image-specific message is invented.
    let (width, height) = dimensions(&bytes)?;
    if width == 0 || height == 0 || width > MAX_SIDE || height > MAX_SIDE {
        return Err(IMAGE_FAILED.into());
    }
    write_temp(run, &bytes)
}

/// The PNG header dimensions, read without decoding the pixels.
fn dimensions(bytes: &[u8]) -> Result<(u32, u32), String> {
    image::ImageReader::new(std::io::Cursor::new(bytes))
        .with_guessed_format()
        .map_err(|_| IMAGE_FAILED.to_string())?
        .into_dimensions()
        .map_err(|_| IMAGE_FAILED.to_string())
}

/// Runs `wl-paste --no-newline --type image/png` with a bounded timeout.
///
/// stdout is drained by its own thread so a large clipboard image cannot fill
/// the pipe and stall the child, which would otherwise turn a slow paste into a
/// timeout. On timeout the child is killed, so nothing is left running.
fn paste() -> Result<Vec<u8>, String> {
    let mut child = Command::new("wl-paste")
        .args(["--no-newline", "--type", "image/png"])
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .map_err(|error| match error.kind() {
            ErrorKind::NotFound => WL_PASTE_MISSING.to_string(),
            _ => format!("cannot run wl-paste: {error}"),
        })?;
    let mut stdout = child.stdout.take().ok_or(WL_PASTE_MISSING)?;
    let (sender, bytes) = mpsc::channel();
    thread::spawn(move || {
        let mut buffer = Vec::new();
        let _ = std::io::Read::read_to_end(&mut stdout, &mut buffer);
        let _ = sender.send(buffer);
    });
    let deadline = Instant::now() + READ_TIMEOUT;
    let status = loop {
        match child.try_wait() {
            Ok(Some(status)) => break status,
            Ok(None) if Instant::now() < deadline => thread::sleep(READ_POLL),
            Ok(None) => {
                let _ = child.kill();
                let _ = child.wait();
                return Err(WL_PASTE_TIMEOUT.into());
            }
            Err(error) => {
                let _ = child.kill();
                let _ = child.wait();
                return Err(format!("cannot run wl-paste: {error}"));
            }
        }
    };
    if !status.success() {
        // A clipboard without an image makes `wl-paste` exit non-zero and print
        // "No selection owner" or similar; the C++ saw a null `QImage` there.
        return Err(NO_IMAGE.into());
    }
    bytes
        .recv_timeout(READ_TIMEOUT)
        .map_err(|_| WL_PASTE_TIMEOUT.into())
}

/// Writes the bytes next to a fresh private file and returns it. A file that
/// cannot be written is the same failure as a failed PNG encode.
///
/// The directory is created exclusively with mode 0700 and the PNG itself with
/// `create_new` and mode 0600, so another local user can neither read the
/// clipboard image nor pre-create a symlink in a predictable path that this
/// process would then write through. The name is not derived from the run id
/// alone: it uses system randomness and retries on a collision.
fn write_temp(_run: u64, bytes: &[u8]) -> Result<TempImage, String> {
    use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};

    let base = env::temp_dir();
    for _ in 0..16 {
        let directory = base.join(format!(
            "deskwaifu-reading-{}-{}",
            std::process::id(),
            unique_suffix()?
        ));
        match fs::DirBuilder::new().mode(0o700).create(&directory) {
            Ok(()) => {}
            Err(error) if error.kind() == ErrorKind::AlreadyExists => continue,
            Err(_) => return Err(IMAGE_FAILED.into()),
        }
        let path = directory.join("image.png");
        let mut file = match fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .open(&path)
        {
            Ok(file) => file,
            Err(_) => {
                let _ = fs::remove_dir(&directory);
                return Err(IMAGE_FAILED.into());
            }
        };
        if std::io::Write::write_all(&mut file, bytes).is_err() || file.sync_all().is_err() {
            let _ = fs::remove_file(&path);
            let _ = fs::remove_dir(&directory);
            return Err(IMAGE_FAILED.into());
        }
        drop(file);
        return Ok(TempImage {
            path: path.to_string_lossy().into_owned(),
            directory,
        });
    }
    Err(IMAGE_FAILED.into())
}

/// A per-call suffix from the Linux system random source.
fn unique_suffix() -> Result<u64, String> {
    use std::io::Read;
    let mut bytes = [0; 8];
    fs::File::open("/dev/urandom")
        .and_then(|mut random| random.read_exact(&mut bytes))
        .map_err(|_| IMAGE_FAILED.to_string())?;
    Ok(u64::from_ne_bytes(bytes))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::unix::fs::{MetadataExt, PermissionsExt};

    #[test]
    fn replacement_before_accepted_ends_reading_without_following_the_new_speech() {
        let root = tempfile::tempdir().unwrap();
        let mut speech = crate::speech::tests::speech_without_worker(root.path());
        let mut reading = Reading::new();
        reading.text = Some("recognized clipboard text".into());
        reading.enter(State::Speaking, Wait::None);
        reading.speech_step(&mut speech);
        let Wait::Speak { token } = reading.wait else {
            panic!("reading must retain the submitted speech token");
        };
        // No worker exists to send Accepted: both ids remain zero throughout
        // the replacement window that the old id-based tracking missed.
        assert_eq!(speech.status()["id"], 0);
        assert_eq!(speech.status()["active"], true);
        speech
            .speak(&json!({"text": "external replacement", "replace": true}))
            .unwrap();
        assert_ne!(speech.current_request_token(), token);
        assert_eq!(speech.status()["id"], 0);
        reading.speech_step(&mut speech);
        assert_eq!(
            reading.status(),
            json!({
                "ok": true, "state": "idle", "error": SPEECH_REPLACED,
            })
        );
        // Fail immediately while the replacement is still pending, not after
        // observing its completion and incorrectly reporting reading success.
        assert_eq!(speech.status()["active"], true);
    }

    #[test]
    fn a_precreated_predictable_directory_symlink_is_not_reused() {
        let target = tempfile::tempdir().unwrap();
        let target_image = target.path().join("image.png");
        fs::write(&target_image, b"must not be replaced").unwrap();
        let run = u64::MAX;
        let planted =
            env::temp_dir().join(format!("deskwaifu-reading-{}-{run}", std::process::id()));
        std::os::unix::fs::symlink(target.path(), &planted).unwrap();
        let result = write_temp(run, b"private clipboard");
        // Only remove the symlink this test created, never its target.
        fs::remove_file(&planted).unwrap();
        let image = result.unwrap();
        assert_ne!(Path::new(&image.path), target_image);
        assert_eq!(fs::read(&target_image).unwrap(), b"must not be replaced");
        assert_eq!(fs::read(&image.path).unwrap(), b"private clipboard");
    }

    #[test]
    fn temporary_images_are_private_independent_and_removed_on_drop() {
        let first = write_temp(1, b"first clipboard").unwrap();
        let second = write_temp(1, b"second clipboard").unwrap();
        assert_ne!(first.directory, second.directory);
        for image in [&first, &second] {
            let directory = fs::symlink_metadata(&image.directory).unwrap();
            let file = fs::symlink_metadata(&image.path).unwrap();
            assert!(directory.is_dir());
            assert!(file.is_file());
            assert_eq!(directory.permissions().mode() & 0o777, 0o700);
            assert_eq!(file.permissions().mode() & 0o777, 0o600);
            assert_eq!(file.nlink(), 1);
        }
        assert_eq!(fs::read(&first.path).unwrap(), b"first clipboard");
        assert_eq!(fs::read(&second.path).unwrap(), b"second clipboard");
        let first_directory = first.directory.clone();
        let second_directory = second.directory.clone();
        drop(first);
        assert!(!first_directory.exists());
        assert_eq!(fs::read(&second.path).unwrap(), b"second clipboard");
        drop(second);
        assert!(!second_directory.exists());
    }
}
