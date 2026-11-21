//! Wayland 文本剪贴板。每次操作独占后端，直到在途进程退出并回收。
use deskwaifu_sdk::{ClipboardWriteError, OsError};
use std::{
    ffi::OsString,
    future::Future,
    process::{ExitStatus, Stdio},
    sync::Arc,
    time::Duration,
};
use tokio::{
    io::{AsyncRead, AsyncReadExt, AsyncWriteExt},
    process::{Child, Command},
    sync::{Mutex, oneshot, watch},
    time::{Instant, sleep_until, timeout_at},
};

const MAX_TEXT: usize = 1024 * 1024;
const MAX_DIAGNOSTIC: usize = 4096;
const OPERATION_TIMEOUT: Duration = Duration::from_secs(5);

#[derive(Default)]
pub(crate) struct Clipboard {
    serial: Arc<Mutex<()>>,
    stopped: watch::Sender<bool>,
    backend: Backend,
}

impl Clipboard {
    pub(crate) async fn read(&self) -> Result<Option<String>, OsError> {
        self.execute(|backend, mut operation| async move { backend.read(&mut operation).await })
            .await
            .map_err(|error| match error {
                ExecutionError::BeforeStart(error) => error,
                ExecutionError::WorkerFailed => OsError::Backend,
            })?
    }

    pub(crate) async fn write(&self, text: String) -> Result<(), ClipboardWriteError> {
        use ClipboardWriteError::{Failed, Unknown};
        if text.len() > MAX_TEXT {
            return Err(Failed(OsError::TooLarge));
        }
        self.execute(|backend, mut operation| async move {
            backend.write(&text, &mut operation).await?;
            match backend.read(&mut operation).await.map_err(Unknown)? {
                Some(read_back) if read_back == text => Ok(()),
                _ => Err(Unknown(OsError::Backend)),
            }
        })
        .await
        .map_err(|error| match error {
            ExecutionError::BeforeStart(error) => Failed(error),
            ExecutionError::WorkerFailed => Unknown(OsError::Backend),
        })?
    }

    async fn execute<T: Send + 'static, F: Future<Output = T> + Send + 'static>(
        &self,
        run: impl FnOnce(Backend, Operation) -> F,
    ) -> Result<T, ExecutionError> {
        let deadline = Instant::now() + self.backend.timeout;
        let serial = timeout_at(deadline, self.serial.clone().lock_owned())
            .await
            .map_err(|_| ExecutionError::BeforeStart(OsError::Timeout))?;
        if *self.stopped.borrow() {
            return Err(ExecutionError::BeforeStart(OsError::Unavailable));
        }
        // 丢弃调用方 Future 会关闭 cancel。后台操作仍持有锁，直到完成清理。
        let (_cancel, cancelled) = oneshot::channel();
        let operation = Operation {
            cancelled,
            stopped: self.stopped.subscribe(),
            deadline,
        };
        let task = run(self.backend.clone(), operation);
        tokio::spawn(async move {
            let _serial = serial;
            task.await
        })
        .await
        .map_err(|_| ExecutionError::WorkerFailed)
    }

    pub(crate) async fn shutdown(&self) {
        self.stopped.send_replace(true);
        let _serial = self.serial.lock().await;
    }
}

enum ExecutionError {
    BeforeStart(OsError),
    WorkerFailed,
}

#[derive(Clone)]
struct Backend {
    paste: OsString,
    copy: OsString,
    timeout: Duration,
}

impl Default for Backend {
    fn default() -> Self {
        Self {
            paste: "wl-paste".into(),
            copy: "wl-copy".into(),
            timeout: OPERATION_TIMEOUT,
        }
    }
}

impl Backend {
    async fn read(&self, operation: &mut Operation) -> Result<Option<String>, OsError> {
        operation.spawn_allowed()?;
        let mut child = command(&self.paste)
            .args(["--no-newline", "--type", "text"])
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .map_err(|_| OsError::Unavailable)?;
        let stdout = child.stdout.take().expect("piped stdout");
        let stderr = child.stderr.take().expect("piped stderr");
        let (status, (text, diagnostics)) = operation
            .run(&mut child, async move {
                tokio::try_join!(read_bounded(stdout, MAX_TEXT), async {
                    read_bounded(stderr, MAX_DIAGNOSTIC)
                        .await
                        .map_err(|_| OsError::Backend)
                })
            })
            .await?;
        decode_paste(status, text, &diagnostics)
    }

    async fn write(
        &self,
        text: &str,
        operation: &mut Operation,
    ) -> Result<(), ClipboardWriteError> {
        use ClipboardWriteError::{Failed, Unknown};
        operation.spawn_allowed().map_err(Failed)?;
        // 后台模式的父进程在设置 selection 后退出。后代继续向其他客户端提供文本。
        let mut child = command(&self.copy)
            .args(["--type", "text/plain;charset=utf-8"])
            .stdin(Stdio::piped())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .map_err(|_| Failed(OsError::Unavailable))?;
        let mut stdin = child.stdin.take().expect("piped stdin");
        let (status, ()) = operation
            .run(&mut child, async move {
                stdin
                    .write_all(text.as_bytes())
                    .await
                    .map_err(|_| OsError::Backend)
            })
            .await
            .map_err(Unknown)?;
        if status.success() {
            Ok(())
        } else {
            Err(Unknown(OsError::Backend))
        }
    }
}

struct Operation {
    cancelled: oneshot::Receiver<()>,
    stopped: watch::Receiver<bool>,
    deadline: Instant,
}

impl Operation {
    fn spawn_allowed(&mut self) -> Result<(), OsError> {
        if *self.stopped.borrow()
            || self.cancelled.try_recv() != Err(oneshot::error::TryRecvError::Empty)
        {
            return Err(OsError::Unavailable);
        }
        if Instant::now() >= self.deadline {
            return Err(OsError::Timeout);
        }
        Ok(())
    }

    async fn run<T>(
        &mut self,
        child: &mut Child,
        io: impl Future<Output = Result<T, OsError>>,
    ) -> Result<(ExitStatus, T), OsError> {
        let result = tokio::select! {
            biased;
            _ = self.stopped.wait_for(|stopped| *stopped) => Err(OsError::Unavailable),
            _ = &mut self.cancelled => Err(OsError::Unavailable),
            _ = sleep_until(self.deadline) => Err(OsError::Timeout),
            result = async {
                let data = io.await?;
                Ok((wait(child).await?, data))
            } => result,
        };
        if result.is_err() {
            terminate(child).await;
        }
        result
    }
}

fn command(program: &OsString) -> Command {
    let mut command = Command::new(program);
    command
        .env("LC_ALL", "C")
        .process_group(0)
        .kill_on_drop(true);
    command
}

async fn wait(child: &mut Child) -> Result<ExitStatus, OsError> {
    child.wait().await.map_err(|_| OsError::Backend)
}

#[allow(unsafe_code)]
async fn terminate(child: &mut Child) {
    if let Some(pid) = child.id() {
        // SAFETY: 启动时建立了独立进程组，仍持有尚未 wait 的组长。wl-paste
        // 可能 fork cat，所以连同组内后代一起终止，再回收直接子进程。
        if unsafe { libc::killpg(pid as libc::pid_t, libc::SIGKILL) } != 0 {
            let _ = child.start_kill();
        }
    }
    let _ = child.wait().await;
}

async fn read_bounded(reader: impl AsyncRead + Unpin, limit: usize) -> Result<Vec<u8>, OsError> {
    let mut bytes = Vec::new();
    reader
        .take(limit as u64 + 1)
        .read_to_end(&mut bytes)
        .await
        .map_err(|_| OsError::Backend)?;
    if bytes.len() > limit {
        return Err(OsError::TooLarge);
    }
    Ok(bytes)
}

fn decode_paste(
    status: ExitStatus,
    text: Vec<u8>,
    diagnostics: &[u8],
) -> Result<Option<String>, OsError> {
    if status.success() && diagnostics.is_empty() {
        return String::from_utf8(text)
            .map(Some)
            .map_err(|_| OsError::InvalidText);
    }
    // wl-paste 对空 selection、缺少文本 MIME 和连接错误都使用退出码 1。
    if status.code() == Some(1)
        && text.is_empty()
        && (diagnostics == b"Nothing is copied\n"
            || diagnostics
                .starts_with(b"Clipboard content is not available as requested type \"text\"\n"))
    {
        return Ok(None);
    }
    Err(OsError::Backend)
}
