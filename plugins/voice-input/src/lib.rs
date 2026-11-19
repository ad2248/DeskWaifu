mod asr;
mod config;

use config::{Config, DEFAULT_ENDPOINT, DEFAULT_MODEL, DEFAULT_SECONDS};
use deskwaifu_sdk::{
    ClipboardWriteError, Context, Message, OsError, OsServices, Plugin, PluginResult,
};
use futures_channel::oneshot;
use futures_util::StreamExt;
use serde_json::{Value, json};
use std::{cell::RefCell, rc::Rc, thread, time::Duration};
use uuid::Uuid;

const MAX_IPC_FRAME: usize = 64 * 1024;
const SETTINGS_REFRESH_INTERVAL: Duration = Duration::from_secs(2);
// 平台服务交付在截止点前收到的完整设备块。
// 允许最后一个设备块跨越采集截止点，同时限制最大录音的内存占用。
const MAX_PCM_BYTES: usize = 301 * asr::PCM_PER_SECOND;

#[derive(Default)]
struct VoiceInput {
    state: Rc<RefCell<State>>,
}

#[derive(Default)]
struct State {
    session: Option<Session>,
    settings: Settings,
    settings_epoch: u64,
    refreshing: bool,
}

struct Settings {
    endpoint: String,
    model: String,
    seconds: u64,
    configured: bool,
    error: Option<&'static str>,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            endpoint: DEFAULT_ENDPOINT.into(),
            model: DEFAULT_MODEL.into(),
            seconds: DEFAULT_SECONDS,
            configured: false,
            error: None,
        }
    }
}

impl Settings {
    fn update(result: &Result<Config, &'static str>) -> Self {
        match result {
            Ok(config) => Self {
                endpoint: config.endpoint.to_string(),
                model: config.model.clone(),
                seconds: config.max_recording_seconds,
                configured: config.authorization.is_some(),
                error: None,
            },
            Err(error) => Self {
                error: Some(error),
                ..Self::default()
            },
        }
    }

    fn json(&self) -> Value {
        json!({"endpoint": self.endpoint, "model": self.model,
            "max_recording_seconds": self.seconds, "api_key_configured": self.configured,
            "error_code": self.error})
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Phase {
    Preparing,
    Recording,
    Recognizing,
    Copying,
    Completed,
    Failed,
}

impl Phase {
    fn name(self) -> &'static str {
        match self {
            Self::Preparing => "preparing",
            Self::Recording => "recording",
            Self::Recognizing => "recognizing",
            Self::Copying => "copying",
            Self::Completed => "completed",
            Self::Failed => "failed",
        }
    }

    fn active(self) -> bool {
        !matches!(self, Self::Completed | Self::Failed)
    }
}

struct Session {
    id: String,
    phase: Phase,
    seconds: Option<u64>,
    text: Option<String>,
    error: Option<&'static str>,
    copy_result: Option<&'static str>,
}

impl State {
    fn status(&self) -> String {
        let session = self.session.as_ref();
        json!({
            "session_id": session.map(|session| &session.id),
            "phase": session.map_or("idle", |session| session.phase.name()),
            "max_recording_seconds": session.and_then(|session| session.seconds),
            "error_code": session.and_then(|session| session.error),
            "copy_result": session.and_then(|session| session.copy_result)
        })
        .to_string()
    }

    fn session(&mut self) -> &mut Session {
        self.session.as_mut().expect("active session")
    }
}

// 网络运行时在插件自身的线程中创建，避免 cdylib 和宿主使用不同的 Tokio 线程局部状态。
// 任务被宿主丢弃时先通知取消，再等待线程退出，动态库卸载后不会继续执行插件代码。
struct Worker<T> {
    result: oneshot::Receiver<T>,
    cancel: Option<oneshot::Sender<()>>,
    thread: Option<thread::JoinHandle<()>>,
}

impl<T: Send + 'static> Worker<T> {
    fn spawn(
        work: impl FnOnce(oneshot::Receiver<()>) -> T + Send + 'static,
    ) -> Result<Self, &'static str> {
        let (cancel, canceled) = oneshot::channel();
        let (send, result) = oneshot::channel();
        let thread = thread::Builder::new()
            .name("voice-input".into())
            .spawn(move || {
                let _ = send.send(work(canceled));
            })
            .map_err(|_| "voice_worker_failed")?;
        Ok(Self {
            result,
            cancel: Some(cancel),
            thread: Some(thread),
        })
    }

    async fn result(mut self) -> Result<T, &'static str> {
        (&mut self.result).await.map_err(|_| "voice_worker_failed")
    }
}

impl<T> Drop for Worker<T> {
    fn drop(&mut self) {
        drop(self.cancel.take());
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

async fn load_config() -> Result<Config, &'static str> {
    let path = config::path();
    Worker::spawn(move |_| Config::load(path))?.result().await?
}

async fn transcribe(config: Config, pcm: Vec<u8>) -> Result<String, &'static str> {
    recognition_worker(asr::recognize(config, pcm))?
        .result()
        .await?
}

fn recognition_worker(
    recognize: impl Future<Output = Result<String, &'static str>> + Send + 'static,
) -> Result<Worker<Result<String, &'static str>>, &'static str> {
    Worker::spawn(move |canceled| {
        let runtime = tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .map_err(|_| "voice_worker_failed")?;
        runtime.block_on(async {
            tokio::select! {
                biased;
                _ = canceled => Err("asr_canceled"),
                result = recognize => result,
            }
        })
    })
}

fn audio_error(error: OsError) -> &'static str {
    match error {
        OsError::Busy => "audio_busy",
        OsError::Overflow => "audio_overflow",
        OsError::Timeout => "audio_timeout",
        OsError::Unavailable => "audio_unavailable",
        _ => "audio_failed",
    }
}

async fn capture(os: &dyn OsServices, seconds: u64) -> Result<Vec<u8>, &'static str> {
    let mut stream = os
        .capture_audio(Duration::from_secs(seconds))
        .await
        .map_err(audio_error)?;
    let mut pcm = Vec::new();
    let max_pcm = ((seconds as usize + 1) * asr::PCM_PER_SECOND).min(MAX_PCM_BYTES);
    while let Some(chunk) = stream.next().await {
        let chunk = chunk.map_err(audio_error)?;
        if chunk.len() & 1 != 0 {
            return Err("invalid_audio");
        }
        if chunk.len() > max_pcm - pcm.len() {
            return Err("audio_overflow");
        }
        pcm.extend_from_slice(&chunk);
    }
    if pcm.is_empty() {
        return Err("no_audio");
    }
    Ok(pcm)
}

async fn copy(os: &dyn OsServices, state: &RefCell<State>, text: String) {
    let result = os.write_clipboard_text(text).await;
    let mut state = state.borrow_mut();
    let session = state.session();
    let (phase, outcome, error) = match result {
        Ok(()) => (Phase::Completed, "copied", None),
        Err(ClipboardWriteError::Failed(_)) => {
            (Phase::Failed, "failed", Some("clipboard_write_failed"))
        }
        Err(ClipboardWriteError::Unknown(_)) => {
            (Phase::Failed, "unknown", Some("clipboard_write_unknown"))
        }
    };
    session.phase = phase;
    session.error = error;
    session.copy_result = Some(outcome);
}

async fn record_and_recognize(
    os: &dyn OsServices,
    state: &RefCell<State>,
) -> Result<String, &'static str> {
    let result = load_config().await;
    state.borrow_mut().settings = Settings::update(&result);
    let config = result?;
    let seconds = config.max_recording_seconds;
    state.borrow_mut().session().seconds = Some(seconds);
    if config.authorization.is_none() {
        return Err("not_configured");
    }
    state.borrow_mut().session().phase = Phase::Recording;
    let pcm = capture(os, seconds).await?;
    state.borrow_mut().session().phase = Phase::Recognizing;
    transcribe(config, pcm).await
}

async fn run_session(os: Rc<dyn OsServices>, state: Rc<RefCell<State>>) {
    match record_and_recognize(os.as_ref(), &state).await {
        Ok(text) => {
            {
                let mut state = state.borrow_mut();
                let session = state.session();
                session.phase = Phase::Copying;
                session.text = Some(text.clone());
            }
            copy(os.as_ref(), &state, text).await;
        }
        Err(error) => {
            let mut state = state.borrow_mut();
            let session = state.session();
            session.phase = Phase::Failed;
            session.error = Some(error);
        }
    }
}

fn session_id(args: &[String]) -> Result<&str, String> {
    match args {
        [option, id] if option == "--session_id" && !id.is_empty() => Ok(id),
        _ => Err("invalid_arguments".into()),
    }
}

fn result_json(session: &Session) -> Result<String, String> {
    let text = session.text.as_ref().ok_or("result_unavailable")?;
    let output = json!({"session_id": session.id, "text": text}).to_string();
    // IPC 的 output 是字符串，外层信封会再次转义引号、反斜杠和换行。
    let frame = json!({"status": "ok", "output": &output}).to_string();
    if frame.len() > MAX_IPC_FRAME {
        return Err("result_too_large_for_ipc".into());
    }
    Ok(output)
}

impl Plugin for VoiceInput {
    fn name(&self) -> &str {
        "voice_input"
    }

    fn initialize(&mut self, cx: &mut Context<'_>) -> PluginResult {
        for (name, help) in [
            ("start_voice_input", "Start a timed voice recording."),
            (
                "get_voice_input_status",
                "Show the most recent voice input session status.",
            ),
            (
                "get_voice_input_result",
                "Get recognized text. Usage: get_voice_input_result --session_id ID",
            ),
            (
                "retry_voice_input_copy",
                "Retry copying recognized text. Usage: retry_voice_input_copy --session_id ID",
            ),
            (
                "get_voice_input_settings",
                "Show voice input settings without credentials.",
            ),
        ] {
            cx.register_command(name, vec![help.into()])?;
        }
        Ok(())
    }

    fn start(&mut self, cx: &mut Context<'_>) -> PluginResult {
        self.tick(cx)
    }

    fn tick_interval(&self) -> Option<Duration> {
        Some(SETTINGS_REFRESH_INTERVAL)
    }

    fn tick(&mut self, cx: &mut Context<'_>) -> PluginResult {
        let mut snapshot = self.state.borrow_mut();
        if snapshot.refreshing {
            return Ok(());
        }
        let epoch = snapshot.settings_epoch;
        let shared = self.state.clone();
        cx.spawn_local(async move {
            let config = load_config().await;
            let mut state = shared.borrow_mut();
            state.refreshing = false;
            if state.settings_epoch == epoch {
                state.settings = Settings::update(&config);
            }
        })?;
        snapshot.refreshing = true;
        Ok(())
    }

    fn on_message(&mut self, _: &Message, _: &mut Context<'_>) -> PluginResult {
        Ok(())
    }

    fn on_command(
        &mut self,
        name: &str,
        args: &[String],
        cx: &mut Context<'_>,
    ) -> Result<String, String> {
        let mut state = self.state.borrow_mut();
        match name {
            "start_voice_input" | "get_voice_input_status" | "get_voice_input_settings"
                if !args.is_empty() =>
            {
                Err("invalid_arguments".into())
            }
            "start_voice_input" => {
                if state
                    .session
                    .as_ref()
                    .is_some_and(|session| session.phase.active())
                {
                    return Err("busy".into());
                }
                cx.spawn_local(run_session(cx.os(), self.state.clone()))?;
                state.settings_epoch = state.settings_epoch.wrapping_add(1);
                state.session = Some(Session {
                    id: Uuid::new_v4().to_string(),
                    phase: Phase::Preparing,
                    seconds: None,
                    text: None,
                    error: None,
                    copy_result: None,
                });
                Ok(state.status())
            }
            "get_voice_input_status" => Ok(state.status()),
            "get_voice_input_settings" => Ok(state.settings.json().to_string()),
            "get_voice_input_result" | "retry_voice_input_copy" => {
                let id = session_id(args)?;
                let session = state
                    .session
                    .as_mut()
                    .filter(|session| session.id == id)
                    .ok_or("session_not_found")?;
                if name == "get_voice_input_result" {
                    return result_json(session);
                }
                if session.phase.active() {
                    return Err("busy".into());
                }
                let text = session.text.clone().ok_or("result_unavailable")?;
                let os = cx.os();
                let shared = self.state.clone();
                cx.spawn_local(async move { copy(os.as_ref(), &shared, text).await })?;
                session.phase = Phase::Copying;
                session.error = None;
                session.copy_result = None;
                Ok(state.status())
            }
            _ => Err("unknown_command".into()),
        }
    }
}

deskwaifu_sdk::export_plugin!(VoiceInput::default());

#[cfg(test)]
mod tests;
