use super::*;
use deskwaifu_sdk::{AudioStream, Command, Host, LocalFuture, PluginId, PublishError};
use futures_util::stream;
use std::{
    any::TypeId, cell::Cell, collections::VecDeque, fs, os::unix::fs::PermissionsExt, path::PathBuf,
};

#[derive(Default)]
struct FakeOs {
    audio: RefCell<VecDeque<Result<Vec<u8>, OsError>>>,
    duration: Cell<Duration>,
    writes: RefCell<Vec<String>>,
    write_error: Cell<Option<ClipboardWriteError>>,
}

impl OsServices for FakeOs {
    fn capture_audio(&self, duration: Duration) -> LocalFuture<'_, Result<AudioStream, OsError>> {
        self.duration.set(duration);
        let chunks = self.audio.take();
        Box::pin(async { Ok(Box::pin(stream::iter(chunks)) as AudioStream) })
    }

    fn read_clipboard_text(&self) -> LocalFuture<'_, Result<Option<String>, OsError>> {
        Box::pin(async { Ok(None) })
    }

    fn write_clipboard_text(
        &self,
        text: String,
    ) -> LocalFuture<'_, Result<(), ClipboardWriteError>> {
        Box::pin(async move {
            self.writes.borrow_mut().push(text);
            self.write_error.take().map_or(Ok(()), Err)
        })
    }
}

#[derive(Default)]
struct TestHost {
    os: Rc<FakeOs>,
    tasks: Vec<LocalFuture<'static, ()>>,
}

impl Host for TestHost {
    fn id(&self) -> PluginId {
        PluginId(0)
    }
    fn subscribe(&mut self, _: TypeId) -> PluginResult {
        Ok(())
    }
    fn publish(&mut self, _: Message) -> Result<(), PublishError> {
        Ok(())
    }
    fn register_command(&mut self, _: Command) -> PluginResult {
        Ok(())
    }
    fn spawn_local(&mut self, task: LocalFuture<'static, ()>) -> PluginResult {
        self.tasks.push(task);
        Ok(())
    }
    fn os(&self) -> Rc<dyn OsServices> {
        self.os.clone()
    }
}

fn sample_session(phase: Phase, text: Option<String>) -> Session {
    Session {
        id: "sample".into(),
        phase,
        seconds: Some(1),
        text,
        error: None,
        copy_result: None,
    }
}

// 测试目的：确认插件按请求时长采集，保留尾部 PCM，并报告采集溢出。
// 测试方法：让 OS 替身先交付常规块和尾块，断言传入时长和拼接结果，再交付溢出错误并检查错误码。
#[tokio::test]
async fn capture_keeps_tail_and_reports_stream_error() {
    let os = FakeOs::default();
    os.audio
        .borrow_mut()
        .extend([Ok(vec![0; 32_000]), Ok(vec![0xFE, 0xCA])]);
    let pcm = capture(&os, 1).await.unwrap();
    assert_eq!(os.duration.get(), Duration::from_secs(1));
    assert_eq!(pcm.len(), 32_002);
    assert_eq!(&pcm[32_000..], &[0xFE, 0xCA]);
    os.audio
        .borrow_mut()
        .extend([Ok(vec![0; 2]), Err(OsError::Overflow)]);
    assert_eq!(capture(&os, 1).await.unwrap_err(), "audio_overflow");
}

// 测试目的：确认活跃会话可查询，重复启动会返回 busy 且只提交一项任务。
// 测试方法：用暂存异步任务的宿主替身依次执行启动、状态查询和再次启动，检查返回值和任务数。
#[test]
fn starting_an_active_session_returns_busy() {
    let mut plugin = VoiceInput::default();
    let mut host = TestHost::default();
    let mut cx = Context::new(&mut host);
    let started = plugin
        .on_command("start_voice_input", &[], &mut cx)
        .unwrap();
    assert_eq!(
        plugin
            .on_command("get_voice_input_status", &[], &mut cx)
            .unwrap(),
        started
    );
    assert_eq!(
        plugin
            .on_command("start_voice_input", &[], &mut cx)
            .unwrap_err(),
        "busy"
    );
    assert_eq!(host.tasks.len(), 1);
}

// 测试目的：确认复制失败或结果未知时仍保留完整转录，并支持按会话重试。
// 测试方法：注入两种剪贴板错误，调用结果查询和重试命令，执行重试任务并核对两次写入及最终复制状态。
#[tokio::test]
async fn copy_errors_keep_text_for_retry() {
    for (error, outcome) in [
        (ClipboardWriteError::Failed(OsError::Backend), "failed"),
        (ClipboardWriteError::Unknown(OsError::Timeout), "unknown"),
    ] {
        let mut plugin = VoiceInput::default();
        let mut host = TestHost::default();
        let text = " 中文\n ";
        plugin.state.borrow_mut().session = Some(sample_session(Phase::Copying, Some(text.into())));
        host.os.write_error.set(Some(error));
        copy(host.os.as_ref(), &plugin.state, text.into()).await;
        assert_eq!(
            plugin.state.borrow_mut().session().copy_result,
            Some(outcome)
        );
        let args = ["--session_id".into(), "sample".into()];
        let mut cx = Context::new(&mut host);
        let result = plugin
            .on_command("get_voice_input_result", &args, &mut cx)
            .unwrap();
        assert_eq!(
            serde_json::from_str::<Value>(&result).unwrap()["text"],
            text
        );
        plugin
            .on_command("retry_voice_input_copy", &args, &mut cx)
            .unwrap();
        host.tasks.pop().unwrap().await;
        assert_eq!(
            plugin.state.borrow_mut().session().copy_result,
            Some("copied")
        );
        assert_eq!(*host.os.writes.borrow(), [text, text]);
    }
}

// 测试目的：确认 IPC 上限按外层 JSON 转义后的字节数计算，超限时保留会话文本。
// 测试方法：用含换行、引号和中文的文本填满 64 KiB 响应，再多加一字节，检查成功结果、错误码和保留的文本。
#[test]
fn result_ipc_budget_counts_outer_escaping() {
    let text = "\n\\\"中文".repeat(3000);
    let mut session = sample_session(Phase::Completed, Some(text));
    let output = result_json(&session).unwrap();
    let frame_bytes = json!({"status": "ok", "output": output}).to_string().len();
    let text = session.text.as_mut().unwrap();
    text.push_str(&"x".repeat(MAX_IPC_FRAME - frame_bytes));
    assert!(result_json(&session).is_ok());
    session.text.as_mut().unwrap().push('x');
    assert_eq!(
        result_json(&session).unwrap_err(),
        "result_too_large_for_ipc"
    );
    assert!(session.text.as_ref().unwrap().ends_with('x'));
}

struct ConfigFile(PathBuf);

impl Drop for ConfigFile {
    fn drop(&mut self) {
        let _ = fs::remove_file(&self.0);
    }
}

// 测试目的：确认配置加载检查权限和录音时长，修复后可重新加载，公开设置只报告密钥配置状态。
// 测试方法：依次加载不存在的文件、权限为 0644 的文件、时长越界的 0600 文件和修复后的文件，核对错误码及公开快照。
#[test]
fn config_requires_private_permissions_and_can_be_reloaded() {
    let file = ConfigFile(
        std::env::temp_dir().join(format!("deskwaifu-voice-config-{}.json", Uuid::new_v4())),
    );
    let read = || Config::load(Some(file.0.clone()));
    let mode = |mode| fs::set_permissions(&file.0, fs::Permissions::from_mode(mode)).unwrap();
    assert!(read().unwrap().authorization.is_none());
    fs::write(
        &file.0,
        r#"{"api_key":"synthetic-key","max_recording_seconds":301}"#,
    )
    .unwrap();
    mode(0o644);
    assert_eq!(read().err(), Some("config_permissions"));
    mode(0o600);
    assert_eq!(read().err(), Some("invalid_config"));
    fs::write(
        &file.0,
        r#"{"api_key":"synthetic-key","max_recording_seconds":2}"#,
    )
    .unwrap();
    let result = read();
    let view = Settings::update(&result).json();
    assert_eq!(view["api_key_configured"], true);
    assert_eq!(view["max_recording_seconds"], 2);
    assert!(!view.to_string().contains("synthetic-key"));
}
