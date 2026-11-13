//! Speech command surface: `get_speech_settings`, `set_speech_settings`, `speak`
//! and `get_speech_status`.
//!
//! This is the JSON-command equivalent of the C++ speech plugin
//! (`plugins/speech/SpeechPlugin.cpp` and `private/processor/speech`). The
//! protocol client is `deskwaifu-speech-domain`, the state machine is its pure
//! [`Player`], and playback is `deskwaifu-audio-output`; this module only wires
//! those three together and mirrors the C++ command semantics.
//!
//! # Replies
//!
//! Both settings commands answer the C++ shape `{ok, configured, error}`;
//! `error` is present and empty on success, exactly like the C++
//! `SpeechPlugin::Command`. `set_speech_settings` accepts `api_key` (a string,
//! empty clears it), writes it through the private [`CredentialStore`] and never
//! returns it. Every validation or storage failure answers the same generic
//! sentence the C++ uses, so a client cannot tell a parse failure from a disk
//! failure, and the stored key is left unchanged.
//!
//! `speak` answers `{ok:true}` once the request is handed to the worker; the
//! assigned id and the machine's progress are observable through
//! `get_speech_status`. Failures use the ordinary `{ok:false, error}` shape with
//! the C++ texts.
//!
//! `get_speech_status` is this runtime's addition, because the C++ exposed speech
//! progress only as internal events: `{ok, active, id, state, configured,
//! last_error}`. `state` is `idle`/`preparing`/`playing`/`draining`, `id` is the
//! most recently accepted speech id (`0` before the first one), `active` says
//! whether that speech is still in flight, and `last_error` is the last failure
//! text or `null`.
//!
//! # Threading
//!
//! One worker thread owns the tokio runtime, the protocol client and the audio
//! player; see [`serve`]. The IPC loop only sends a [`Command`] and drains
//! [`Report`]s on its existing 16 ms tick, so no request ever blocks the accept
//! path and no audio or network work runs there. One active speech at a time:
//! `speak` either replaces the current one or fails with the C++ busy text.
//!
//! # Completion and the cloud boundary
//!
//! `Finished(Completed)` is produced only after `deskwaifu-audio-output` reports
//! `Drained` for the active id (SP-04); a source `Completed` event merely enters
//! `Draining`. With a key configured, `speak` sends a real request to the fixed
//! MiniMax endpoint over `wss://`; without network or credentials the failure is
//! reported as it happens and never as a fake success.
//!
//! **Untested in CI:** the automated suite only drives the loopback
//! `--speech-endpoint` seam plus the silent in-process sink behind
//! `DESKWAIFU_SPEECH_TEST_SINK=1`. A real MiniMax synthesis and a real audio
//! device are not exercised by any automated test.

use deskwaifu_audio_output::{
    Converter, CpalFactory, DeviceFormat, Error as AudioError, ErrorCode, Event as AudioEvent,
    Player as AudioPlayer, Progress, Queue, SampleFormat, Shared, Sink, SinkFactory, SourceFormat,
};
use deskwaifu_config::settings::ConfigDocument;
use deskwaifu_config::CredentialStore;
use deskwaifu_speech_domain::player::{Outcome, Player, PlayerError, State};
use deskwaifu_speech_domain::{
    synthesize, Error as SourceError, Event as SourceEvent, Events, SpeechRequest,
};
use serde_json::{json, Value};
use std::env;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver, Sender};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::thread::JoinHandle;
use std::time::Duration;

/// The endpoint the C++ `MiniMaxSpeechSource` fixes.
const DEFAULT_ENDPOINT: &str = "wss://api.minimax.cn/ws/v1/t2a_v2_bidi";
/// The C++ response timeout: a gap between valid service messages, not a total
/// budget for one playback.
const TIMEOUT: Duration = Duration::from_secs(30);
/// The `SpeechOptions` defaults.
const VOICE_ID: &str = "female-tianmei";
const MODEL: &str = "speech-2.8-turbo";
/// C++ `text.size() >= 10000`, where `QString::size()` counts UTF-16 code units.
const MAX_TEXT_UNITS: usize = 10_000;
/// How long one iteration may wait for a source event, so `pump` keeps a
/// sub-frame cadence without a separate timer thread.
const POLL: Duration = Duration::from_millis(2);
const SOURCE_EVENT_BUDGET: usize = 32;
/// The C++ `SpeechProcessor` texts.
const EMPTY_TEXT: &str = "speech text is empty";
const BUSY: &str = "speech service is busy";
const STOPPED: &str = "speech service stopped";
const INVALID_REQUEST: &str = "Invalid speech request";
/// The C++ `MiniMaxSpeechSource` text for an unusable text or parameter set.
const INVALID_TEXT: &str = "无效的语音文本或参数";
/// The C++ text when the private settings cannot be read or updated.
const SETTINGS_FAILED: &str = "Unable to read or update speech settings";
const SETTINGS_READ_FAILED: &str = "Unable to read speech settings";
/// The C++ `SpeechPlugin` text for a missing key.
const UNCONFIGURED: &str = "请先在控制面板中设置 MiniMax API Key";

/// The single-slot mailbox shared by the IPC loop and the worker.
///
/// It is a slot rather than a queue because only one speech may be active: a
/// replacing request overwrites whatever was not picked up yet, so the loop can
/// never hand the worker more than one pending request and no queue can grow.
struct Mailbox {
    slot: Mutex<Option<Command>>,
    ready: Condvar,
    closed: AtomicBool,
}

impl Mailbox {
    fn new() -> Self {
        Self {
            slot: Mutex::new(None),
            ready: Condvar::new(),
            closed: AtomicBool::new(false),
        }
    }

    fn lock(&self) -> MutexGuard<'_, Option<Command>> {
        self.slot.lock().unwrap_or_else(|error| error.into_inner())
    }

    /// Publishes a command, replacing any unpicked one. `false` once closed.
    fn send(&self, command: Command) -> bool {
        if self.closed.load(Ordering::Relaxed) {
            return false;
        }
        *self.lock() = Some(command);
        self.ready.notify_one();
        true
    }

    /// Waits up to `timeout` for a command; `None` means none arrived.
    fn take(&self, timeout: Duration) -> Option<Command> {
        let slot = self.lock();
        let (mut slot, _) = self
            .ready
            .wait_timeout_while(slot, timeout, |slot| {
                slot.is_none() && !self.closed.load(Ordering::Relaxed)
            })
            .unwrap_or_else(|error| error.into_inner());
        slot.take()
    }

    fn closed(&self) -> bool {
        self.closed.load(Ordering::Relaxed)
    }

    /// Ends the worker loop; the pending command, if any, is dropped.
    fn close(&self) {
        self.closed.store(true, Ordering::Relaxed);
        self.ready.notify_all();
    }
}
/// One unit of work for the worker thread: a validated request and the
/// credential to use for it.
struct Command {
    token: u64,
    text: String,
    key: String,
}

/// What the worker reports back to the IPC loop.
enum Report {
    /// The state machine allocated an id for a request token.
    Accepted { token: u64, id: u64 },
    /// The state machine entered a state for a request token.
    StateChanged { token: u64, state: State },
    /// A request ended; exactly one per accepted token.
    Finished { token: u64 },
    /// A request failed; the text never contains the credential.
    Error { token: u64, message: String },
}

/// The settings and speech state the IPC loop owns. Audio and network work lives
/// in the worker thread, not here.
pub struct Speech {
    store: CredentialStore,
    endpoint: String,
    api_key: String,
    configured: bool,
    settings_error: bool,
    worker: Option<Worker>,
    id: u64,
    state: State,
    active: bool,
    last_error: Option<String>,
    request_token: u64,
    next_token: u64,
}

impl Speech {
    /// Reads the private credential file once and starts nothing else; the worker
    /// thread appears on the first accepted `speak`.
    pub fn new(endpoint: String) -> Self {
        let store = CredentialStore::new(config_root(), "speech");
        let (api_key, settings_error) = match read_key(&store) {
            Ok(key) => (key, false),
            Err(()) => (String::new(), true),
        };
        let configured = !api_key.is_empty();
        Self {
            store,
            endpoint,
            api_key,
            configured,
            settings_error,
            worker: None,
            id: 0,
            state: State::Idle,
            active: false,
            last_error: None,
            request_token: 0,
            next_token: 1,
        }
    }

    /// `get_speech_settings`: re-read the private file and answer the cached
    /// state, like the C++ `Refresh` followed by the reply.
    pub fn settings(&mut self) -> Value {
        self.refresh();
        json!({
            "ok": !self.settings_error,
            "configured": self.configured,
            "error": if self.settings_error { SETTINGS_FAILED } else { "" },
        })
    }

    /// `set_speech_settings`: validate and persist `api_key`. Every failure keeps
    /// the cached key, answers the generic sentence and leaves the file alone,
    /// as the C++ `SpeechSettings::Set` does.
    ///
    /// `raw` is the request document as received. The C++ checks it for duplicate
    /// keys *before* `QJsonObject` can collapse them, so a duplicated `api_key`
    /// could otherwise persist the wrong value while reporting success.
    pub fn configure(&mut self, raw: &[u8], args: &Value) -> Value {
        let unique = std::str::from_utf8(raw)
            .map_err(|_| ())
            .and_then(|text| {
                ConfigDocument::parse(text, false)
                    .map(|_| ())
                    .map_err(|_| ())
            })
            .is_ok();
        let key = args
            .get("api_key")
            .and_then(Value::as_str)
            .filter(|key| key.len() <= 4096 && !key.bytes().any(|byte| byte.is_ascii_control()));
        let ok = unique && key.is_some_and(|key| self.store.set("api_key", key).is_ok());
        if ok {
            let key = key.unwrap_or_default();
            self.api_key = key.to_owned();
            self.configured = !key.is_empty();
            self.settings_error = false;
        }
        // A failed write is not a failed read: `configured` keeps describing the
        // last successfully applied key, and a later read reports the file's own
        // state, exactly like the C++ `Refresh`.
        json!({
            "ok": ok,
            "configured": self.configured,
            "error": if ok { "" } else { SETTINGS_FAILED },
        })
    }

    /// `speak`: validate, refresh the credential, then hand one request to the
    /// worker. Rejections use the C++ texts and never disturb the current
    /// playback.
    pub fn speak(&mut self, args: &Value) -> Result<Value, String> {
        self.speak_with_token(args).map(|_| json!({"ok": true}))
    }

    pub(crate) fn speak_with_token(&mut self, args: &Value) -> Result<u64, String> {
        let text = args
            .get("text")
            .and_then(Value::as_str)
            .ok_or(INVALID_REQUEST)?;
        let replace = match args.get("replace") {
            None => false,
            Some(Value::Bool(replace)) => *replace,
            Some(_) => return Err(INVALID_REQUEST.into()),
        };
        // A blank request must not interrupt the current playback.
        if text.trim().is_empty() {
            return Err(EMPTY_TEXT.into());
        }
        // Divergence from the C++: the source there rejects an over-long text
        // asynchronously, after a `replace` already cancelled the current
        // playback. Rejecting it here is what "a rejected request does not
        // disturb playback" implies.
        if text.encode_utf16().count() >= MAX_TEXT_UNITS {
            return Err(INVALID_TEXT.into());
        }
        self.refresh();
        if self.settings_error {
            return Err(SETTINGS_READ_FAILED.into());
        }
        if !self.configured {
            return Err(UNCONFIGURED.into());
        }
        self.poll();
        if self.active && !replace {
            return Err(BUSY.into());
        }
        let token = self.next_token;
        self.next_token = self.next_token.wrapping_add(1).max(1);
        let command = Command {
            token,
            text: text.to_owned(),
            key: self.api_key.clone(),
        };
        let worker = self
            .worker
            .get_or_insert_with(|| Worker::start(&self.endpoint));
        if !worker.mailbox.send(command) {
            return Err(STOPPED.into());
        }
        // The machine assigns the id, so the reply cannot carry it yet; a
        // replacing request keeps the old id until the worker reports the new
        // one, which `get_speech_status` resolves within a tick.
        self.active = true;
        self.last_error = None;
        self.request_token = token;
        Ok(token)
    }

    /// `get_speech_status`: the loop-side view, drained first so a client never
    /// reads a stale state.
    pub fn status(&mut self) -> Value {
        self.poll();
        json!({
            "ok": true,
            "active": self.active,
            "id": self.id,
            "state": state_name(self.state),
            "configured": self.configured,
            "last_error": self.last_error,
        })
    }

    /// Applies the worker reports queued since the last tick. Never blocks.
    pub fn poll(&mut self) {
        let mut reports = Vec::new();
        if let Some(worker) = self.worker.as_ref() {
            while let Ok(report) = worker.reports.try_recv() {
                reports.push(report);
            }
        }
        for report in reports {
            match report {
                Report::Accepted { token, id } => {
                    if token == self.request_token {
                        self.id = id;
                        self.active = true;
                        self.last_error = None;
                    }
                }
                Report::StateChanged { token, state } => {
                    if token == self.request_token {
                        self.state = state;
                    }
                }
                Report::Finished { token } => {
                    if token == self.request_token {
                        self.active = false;
                    }
                }
                Report::Error { token, message } => {
                    if token == self.request_token {
                        self.active = false;
                        self.last_error = Some(message);
                    }
                }
            }
        }
    }

    pub(crate) fn current_request_token(&mut self) -> u64 {
        self.poll();
        self.request_token
    }

    /// Re-reads the private file. A failed read keeps the cached key, exactly
    /// like the C++ `Refresh`, but is remembered so `ok` stays honest.
    fn refresh(&mut self) {
        match read_key(&self.store) {
            Ok(key) => {
                self.api_key = key;
                self.configured = !self.api_key.is_empty();
                self.settings_error = false;
            }
            Err(()) => self.settings_error = true,
        }
    }
}

fn state_name(state: State) -> &'static str {
    match state {
        State::Idle => "idle",
        State::Preparing => "preparing",
        State::Playing => "playing",
        State::Draining => "draining",
    }
}

/// `<configRoot>`, i.e. what Qt calls `QStandardPaths::ConfigLocation`, so the
/// file lands where the C++ `SpeechSettings` keeps it.
fn config_root() -> PathBuf {
    env::var_os("XDG_CONFIG_HOME")
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
        .or_else(|| env::var_os("HOME").map(|home| PathBuf::from(home).join(".config")))
        .unwrap_or_else(|| PathBuf::from(".config"))
}

/// Reads `api_key` with the C++ `SpeechSettings::Decode` rules: a missing file or
/// field is an empty key, while a value that is not a JSON string, is longer than
/// 4096 bytes or contains an ASCII control character fails the whole read.
/// CredentialStore parses comments and duplicate keys consistently for both
/// reads and updates; this layer adds the C++ speech setting's string validation.
fn read_key(store: &CredentialStore) -> Result<String, ()> {
    let Some(value) = store.get("api_key").map_err(|_| ())? else {
        return Ok(String::new());
    };
    let key = value.as_str().ok_or(())?;
    if key.len() > 4096 || key.bytes().any(|byte| byte.is_ascii_control()) {
        return Err(());
    }
    Ok(key.to_owned())
}

struct Worker {
    mailbox: Arc<Mailbox>,
    reports: Receiver<Report>,
    thread: Option<JoinHandle<()>>,
}

impl Worker {
    fn start(endpoint: &str) -> Self {
        let mailbox = Arc::new(Mailbox::new());
        let (outgoing, reports) = mpsc::channel();
        let endpoint = endpoint.to_owned();
        let worker_mailbox = Arc::clone(&mailbox);
        let thread = std::thread::spawn(move || serve(&endpoint, &worker_mailbox, &outgoing));
        Self {
            mailbox,
            reports,
            thread: Some(thread),
        }
    }
}

impl Drop for Worker {
    fn drop(&mut self) {
        // Closing the mailbox ends the worker loop, which stops the audio stream
        // and drops the runtime on its own thread.
        self.mailbox.close();
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

/// The audio seam. `deskwaifu-audio-output` is generic over its [`SinkFactory`],
/// so the runtime picks the real cpal device or the silent test sink.
enum Device {
    Cpal(CpalFactory),
    Silent(SilentFactory),
}

impl SinkFactory for Device {
    fn open(&mut self, source: SourceFormat, queue: Shared) -> Result<Box<dyn Sink>, AudioError> {
        match self {
            Device::Cpal(factory) => factory.open(source, queue),
            Device::Silent(factory) => factory.open(source, queue),
        }
    }
}

/// `DESKWAIFU_SPEECH_TEST_SINK=1` replaces the output device with a thread that
/// pulls the queue the way a device callback would. It is a test/diagnostic seam
/// only; the default is the real device. `DESKWAIFU_SPEECH_TEST_SINK_DELAY_MS`
/// slows that thread down so a test can observe `draining` before completion.
fn device() -> Device {
    if env::var_os("DESKWAIFU_SPEECH_TEST_SINK").is_some_and(|value| value == "1") {
        Device::Silent(SilentFactory)
    } else {
        Device::Cpal(CpalFactory)
    }
}

fn silent_delay() -> Duration {
    env::var("DESKWAIFU_SPEECH_TEST_SINK_DELAY_MS")
        .ok()
        .and_then(|value| value.parse::<u64>().ok())
        .map(Duration::from_millis)
        .unwrap_or(POLL)
}

/// Pulls 10 ms of audio every 2 ms and discards it, so drain, conversion and
/// event ordering are exercised without hardware.
struct SilentFactory;

impl SinkFactory for SilentFactory {
    fn open(&mut self, source: SourceFormat, queue: Shared) -> Result<Box<dyn Sink>, AudioError> {
        if env::var_os("DESKWAIFU_SPEECH_TEST_SINK_FAIL").is_some_and(|value| value == "1") {
            return Err(AudioError::NO_DEVICE);
        }
        let format = DeviceFormat {
            sample_rate: source.sample_rate,
            channels: 2,
            sample_format: SampleFormat::F32,
        };
        let converter = Converter::new(source, format).ok_or(AudioError::NO_DEVICE)?;
        let progress = Arc::new(Progress::new());
        let callback_progress = Arc::clone(&progress);
        let stop = Arc::new(AtomicBool::new(false));
        let callback_stop = Arc::clone(&stop);
        let delay = silent_delay();
        let thread = std::thread::spawn(move || {
            let mut buffer = vec![0u8; 240 * format.frame_bytes()];
            while !callback_stop.load(Ordering::Relaxed) {
                callback_progress.begin_callback();
                // One step for the drain check, exactly like the real sink.
                callback_progress.begin_fill();
                let filled = {
                    let mut queue = queue.lock().unwrap_or_else(|error| error.into_inner());
                    Queue::fill(&mut queue, &converter, &mut buffer)
                };
                callback_progress.delivered(filled as u64);
                callback_progress.end_fill();
                std::thread::sleep(delay);
            }
        });
        Ok(Box::new(SilentSink {
            format,
            progress,
            stop,
            thread: Some(thread),
        }))
    }
}

struct SilentSink {
    format: DeviceFormat,
    progress: Arc<Progress>,
    stop: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
}

impl Drop for SilentSink {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

impl Sink for SilentSink {
    fn format(&self) -> DeviceFormat {
        self.format
    }

    fn progress(&self) -> Arc<Progress> {
        Arc::clone(&self.progress)
    }
}

/// The worker body: one tokio runtime, one pure state machine, one audio player.
/// The network is never awaited unconditionally: the protocol client runs as a
/// runtime task and its events are polled with a short timeout, which leaves the
/// loop free to pump the audio player like the C++ 10 ms timer.
fn serve(endpoint: &str, mailbox: &Mailbox, reports: &Sender<Report>) {
    let runtime = match tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
    {
        Ok(runtime) => runtime,
        Err(_) => return,
    };
    let mut machine = Player::new();
    let mut audio = AudioPlayer::with_factory(device());
    let mut source: Option<Events> = None;
    let mut active_token = 0u64;
    loop {
        // While anything is in flight, wake every `POLL` (the C++ pump cadence);
        // when truly idle, wait longer so the thread does not wake for nothing.
        let idle = source.is_none() && audio.id() == 0 && machine.id() == 0;
        let timeout = if idle {
            Duration::from_millis(10)
        } else {
            POLL
        };
        let command = mailbox.take(timeout);
        if mailbox.closed() && command.is_none() {
            break;
        }
        if let Some(Command { token, text, key }) = command {
            // A replacing request finishes the old id before allocating a new
            // one; the old audio stream is stopped and the old receiver dropped,
            // which cancels its socket task.
            let previous = machine.id();
            if let Some(id) = machine.speak(&text) {
                settle(
                    &mut machine,
                    &mut audio,
                    previous,
                    active_token,
                    token,
                    reports,
                );
                active_token = token;
                source = Some(runtime.block_on(synthesize(
                    endpoint.to_owned(),
                    key,
                    SpeechRequest {
                        id,
                        text,
                        voice_id: VOICE_ID.into(),
                        model: MODEL.into(),
                        speed: 1.0,
                        volume: 1.0,
                        pitch: 0,
                    },
                    TIMEOUT,
                )));
            }
        }
        // Drive the source. A closed channel means this source ended, whether it
        // reported `Completed`, a failure or was dropped; none of those is a
        // fresh failure here.
        let mut ended = false;
        if let Some(events) = source.as_mut() {
            let (batch, closed) =
                runtime.block_on(source_batch(events, tokio::time::Instant::now() + POLL));
            ended = closed;
            for result in batch {
                let event = match result {
                    Ok(event) => event,
                    Err(error) => {
                        let previous = machine.id();
                        fail(&mut machine, PlayerError::Source(error));
                        settle(
                            &mut machine,
                            &mut audio,
                            previous,
                            active_token,
                            active_token,
                            reports,
                        );
                        ended = true;
                        break;
                    }
                };
                let previous = machine.id();
                apply(&mut machine, &mut audio, event);
                settle(
                    &mut machine,
                    &mut audio,
                    previous,
                    active_token,
                    active_token,
                    reports,
                );
            }
        }
        if ended {
            source = None;
        }
        audio.pump();
        for event in audio.take_events() {
            let previous = machine.id();
            match event {
                AudioEvent::PlaybackStarted(id) => machine.started(id),
                AudioEvent::Drained(id) => machine.drained(id),
                AudioEvent::Failed(id, error) => {
                    machine.failed(id, audio_error(error.code));
                    source = None;
                }
            }
            settle(
                &mut machine,
                &mut audio,
                previous,
                active_token,
                active_token,
                reports,
            );
        }
        // Dropping the receiver only wakes the source task's `tx.closed()`.
        // A current-thread runtime must still run that task to release its socket,
        // including after an audio failure leaves this worker otherwise idle.
        runtime.block_on(async { tokio::time::sleep(POLL).await });
    }
    // Never leave a stream running or an id current once the channel closes.
    machine.stop();
    emit(&mut machine, active_token, active_token, reports);
    let id = audio.id();
    audio.stop(id);
}

async fn source_batch(
    events: &mut Events,
    deadline: tokio::time::Instant,
) -> (Vec<Result<SourceEvent, SourceError>>, bool) {
    let mut batch = Vec::with_capacity(SOURCE_EVENT_BUDGET);
    let mut closed = false;
    for _ in 0..SOURCE_EVENT_BUDGET {
        match tokio::time::timeout_at(deadline, events.recv()).await {
            Err(_) => break,
            Ok(None) => {
                closed = true;
                break;
            }
            Ok(Some(event)) => batch.push(event),
        }
    }
    (batch, closed)
}

fn audio_error(code: ErrorCode) -> PlayerError {
    match code {
        ErrorCode::BufferOverflow => PlayerError::BufferOverflow,
        ErrorCode::AudioDevice | ErrorCode::Protocol => PlayerError::AudioDevice,
    }
}

/// Applies one source event to the pipeline, migrating the C++ `SpeechPlayer`
/// connections: `Ready` starts audio for the active id, `Audio` appends, and
/// `Completed` ends the input (the machine enters `Draining`, never `Completed`).
fn apply(machine: &mut Player, audio: &mut AudioPlayer<Device>, event: SourceEvent) {
    match event {
        SourceEvent::Ready {
            sample_rate,
            channels,
            ..
        } => {
            let id = machine.id();
            if machine.ready(id) {
                audio.start(
                    id,
                    SourceFormat {
                        sample_rate,
                        channels,
                    },
                );
            }
        }
        SourceEvent::Audio(pcm) => {
            let id = machine.id();
            audio.append(id, &pcm);
        }
        SourceEvent::Completed => {
            let id = machine.id();
            machine.completed(id);
            audio.finish_input(id);
        }
    }
}

fn fail(machine: &mut Player, error: PlayerError) {
    let id = machine.id();
    machine.failed(id, error);
}

/// Emits the machine's outcomes and keeps the audio player aligned with it: the
/// C++ `End` cancels the source and stops the audio output for the id it just
/// cleared, which is what makes a replaced or failed playback release its stream.
fn settle(
    machine: &mut Player,
    audio: &mut AudioPlayer<Device>,
    previous: u64,
    previous_token: u64,
    token: u64,
    reports: &Sender<Report>,
) {
    emit(machine, previous_token, token, reports);
    let current = machine.id();
    if previous != 0 && previous != current {
        audio.stop(previous);
    }
}

/// Turns the machine's outcomes into loop reports. `Finished` carries no id
/// because the loop tracks exactly one active speech.
fn emit(machine: &mut Player, previous_token: u64, token: u64, reports: &Sender<Report>) {
    for outcome in machine.take_outcomes() {
        let report = match outcome {
            Outcome::Accepted { id } => Report::Accepted { token, id },
            Outcome::StateChanged(state) => Report::StateChanged {
                token: if state == State::Idle {
                    previous_token
                } else {
                    token
                },
                state,
            },
            Outcome::Finished { .. } => Report::Finished {
                token: previous_token,
            },
            Outcome::Error { error, .. } => Report::Error {
                token,
                message: message(error),
            },
        };
        let _ = reports.send(report);
    }
}

/// The C++ `SpeechError` texts, without service text or credentials.
fn message(error: PlayerError) -> String {
    match error {
        PlayerError::Source(error) => source_message(error),
        PlayerError::AudioDevice => "音频输出设备发生错误".into(),
        PlayerError::BufferOverflow => "语音播放缓冲区已满".into(),
    }
}

fn source_message(error: SourceError) -> String {
    match error {
        SourceError::InvalidRequest => INVALID_TEXT.into(),
        SourceError::MissingApiKey => UNCONFIGURED.into(),
        SourceError::Network => "语音连接失败".into(),
        SourceError::Timeout => "语音服务响应超时".into(),
        SourceError::Protocol => "无效的语音服务响应".into(),
        SourceError::Authentication => "语音服务拒绝请求（代码 1004）".into(),
        SourceError::Synthesis(code) => format!("语音服务拒绝请求（代码 {code}）"),
    }
}

/// The production endpoint. `--speech-endpoint` overrides it for tests and
/// diagnostics only, since the C++ fixes it in `MiniMaxSpeechSource`.
pub fn default_endpoint() -> &'static str {
    DEFAULT_ENDPOINT
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use std::fs;

    /// A configured command surface with no network, audio, or worker thread.
    pub(crate) fn speech_without_worker(root: &std::path::Path) -> Speech {
        speech_with_reports(root).0
    }

    fn speech_with_reports(root: &std::path::Path) -> (Speech, Sender<Report>) {
        let store = CredentialStore::new(root, "speech");
        store.set("api_key", "test-key").unwrap();
        let (outgoing, reports) = mpsc::channel();
        let speech = Speech {
            store,
            endpoint: "ws://unused.invalid".into(),
            api_key: "test-key".into(),
            configured: true,
            settings_error: false,
            worker: Some(Worker {
                mailbox: Arc::new(Mailbox::new()),
                reports,
                thread: None,
            }),
            id: 0,
            state: State::Idle,
            active: false,
            last_error: None,
            request_token: 0,
            next_token: 1,
        };
        (speech, outgoing)
    }

    #[tokio::test]
    async fn source_batch_is_bounded_and_leaves_the_worker_iteration_for_mailbox() {
        let (sender, mut events) = tokio::sync::mpsc::channel(SOURCE_EVENT_BUDGET + 8);
        for _ in 0..(SOURCE_EVENT_BUDGET + 8) {
            sender.try_send(Ok(SourceEvent::Completed)).unwrap();
        }

        let (batch, closed) = source_batch(&mut events, tokio::time::Instant::now() + POLL).await;
        assert_eq!(batch.len(), SOURCE_EVENT_BUDGET);
        assert!(!closed);
        assert_eq!(events.len(), 8);

        let mailbox = Mailbox::new();
        assert!(mailbox.send(Command {
            token: 7,
            text: "replacement".into(),
            key: "test-key".into(),
        }));
        let command = mailbox
            .take(Duration::ZERO)
            .expect("mailbox remains serviceable");
        assert_eq!(command.token, 7);
        assert_eq!(command.text, "replacement");
        assert_eq!(
            events.len(),
            8,
            "remaining events belong to the next iteration"
        );
    }

    #[test]
    fn stale_reports_cannot_release_or_overwrite_a_pending_replacement() {
        let root = tempfile::tempdir().unwrap();
        let (mut speech, reports) = speech_with_reports(root.path());
        let a = speech.speak_with_token(&json!({"text": "A"})).unwrap();
        let b = speech
            .speak_with_token(&json!({"text": "B", "replace": true}))
            .unwrap();
        assert_ne!(a, b);
        for report in [
            Report::Accepted { token: a, id: 91 },
            Report::StateChanged {
                token: a,
                state: State::Playing,
            },
            Report::Error {
                token: a,
                message: "stale failure".into(),
            },
            Report::Finished { token: a },
        ] {
            reports.send(report).unwrap();
            assert_eq!(
                speech.status(),
                json!({
                    "ok": true, "active": true, "id": 0, "state": "idle",
                    "configured": true, "last_error": null,
                })
            );
            assert_eq!(speech.current_request_token(), b);
            assert_eq!(speech.speak(&json!({"text": "C"})), Err(BUSY.into()));
            let slot = speech.worker.as_ref().unwrap().mailbox.lock();
            let command = slot.as_ref().unwrap();
            assert_eq!(command.token, b);
            assert_eq!(command.text, "B");
        }
        // Reports for B must still apply; ignoring every report is not a fix.
        reports.send(Report::Accepted { token: b, id: 92 }).unwrap();
        reports
            .send(Report::StateChanged {
                token: b,
                state: State::Preparing,
            })
            .unwrap();
        assert_eq!(speech.status()["id"], 92);
        assert_eq!(speech.status()["state"], "preparing");
        reports.send(Report::Finished { token: b }).unwrap();
        assert_eq!(speech.status()["active"], false);
    }

    #[test]
    fn emit_preserves_token_ownership_across_replacement_and_failure() {
        let (sender, reports) = mpsc::channel();
        let mut machine = Player::new();
        let a = 11;
        let b = 22;
        let first = machine.speak("A").unwrap();
        emit(&mut machine, 0, a, &sender);
        assert!(
            matches!(reports.try_recv().unwrap(), Report::Accepted { token, id } if token == a && id == first)
        );
        assert!(
            matches!(reports.try_recv().unwrap(), Report::StateChanged { token, state: State::Preparing } if token == a)
        );
        assert!(reports.try_recv().is_err());

        let second = machine.speak("B").unwrap();
        emit(&mut machine, a, b, &sender);
        assert!(matches!(reports.try_recv().unwrap(), Report::Finished { token } if token == a));
        assert!(
            matches!(reports.try_recv().unwrap(), Report::Accepted { token, id } if token == b && id == second)
        );
        assert!(
            matches!(reports.try_recv().unwrap(), Report::StateChanged { token, state: State::Preparing } if token == b)
        );
        assert!(reports.try_recv().is_err());

        machine.failed(second, PlayerError::AudioDevice);
        emit(&mut machine, b, b, &sender);
        assert!(
            matches!(reports.try_recv().unwrap(), Report::Error { token, message: text } if token == b && text == message(PlayerError::AudioDevice))
        );
        assert!(matches!(reports.try_recv().unwrap(), Report::Finished { token } if token == b));
        assert!(
            matches!(reports.try_recv().unwrap(), Report::StateChanged { token, state: State::Idle } if token == b)
        );
        assert!(reports.try_recv().is_err());
    }

    #[test]
    fn state_names_match_the_client_vocabulary() {
        assert_eq!(state_name(State::Idle), "idle");
        assert_eq!(state_name(State::Preparing), "preparing");
        assert_eq!(state_name(State::Playing), "playing");
        assert_eq!(state_name(State::Draining), "draining");
    }

    #[test]
    fn credential_reader_accepts_cpp_json_comments() {
        let root = tempfile::tempdir().unwrap();
        let store = CredentialStore::new(root.path(), "speech");
        fs::create_dir_all(store.path().parent().unwrap()).unwrap();
        fs::write(
            store.path(),
            br#"{
                // The legacy C++ parser accepts comments.
                "api_key": "key//part" /* retained as a string */
            }"#,
        )
        .unwrap();
        assert_eq!(read_key(&store).unwrap(), "key//part");
    }

    #[test]
    fn credential_reader_still_rejects_duplicate_keys_with_comments() {
        let root = tempfile::tempdir().unwrap();
        let store = CredentialStore::new(root.path(), "speech");
        fs::create_dir_all(store.path().parent().unwrap()).unwrap();
        fs::write(
            store.path(),
            br#"{
                "api_key": "first", // comments must not bypass duplicate checks
                "api_\u006bey": "second"
            }"#,
        )
        .unwrap();
        assert!(read_key(&store).is_err());
    }

    #[test]
    fn source_failures_use_the_cpp_texts() {
        assert_eq!(
            message(PlayerError::Source(SourceError::Network)),
            "语音连接失败"
        );
        assert_eq!(
            message(PlayerError::Source(SourceError::Timeout)),
            "语音服务响应超时"
        );
        assert_eq!(
            message(PlayerError::Source(SourceError::Protocol)),
            "无效的语音服务响应"
        );
        assert_eq!(
            message(PlayerError::Source(SourceError::Authentication)),
            "语音服务拒绝请求（代码 1004）"
        );
        assert_eq!(
            message(PlayerError::Source(SourceError::Synthesis(2013))),
            "语音服务拒绝请求（代码 2013）"
        );
        assert_eq!(message(PlayerError::AudioDevice), "音频输出设备发生错误");
        assert_eq!(message(PlayerError::BufferOverflow), "语音播放缓冲区已满");
    }

    #[test]
    fn audio_failures_map_onto_the_player_error_split() {
        assert_eq!(
            audio_error(ErrorCode::AudioDevice),
            PlayerError::AudioDevice
        );
        assert_eq!(audio_error(ErrorCode::Protocol), PlayerError::AudioDevice);
        assert_eq!(
            audio_error(ErrorCode::BufferOverflow),
            PlayerError::BufferOverflow
        );
    }
}
