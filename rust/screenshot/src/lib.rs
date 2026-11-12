//! Explicit, interactive XDG Desktop Portal screenshots; no image decoder or capture fallback.
use futures_util::StreamExt;
use std::{
    collections::HashMap,
    path::PathBuf,
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        Arc, Mutex,
    },
    time::Duration,
};
use tokio::{
    io::AsyncReadExt,
    sync::oneshot,
    time::{timeout_at, Instant},
};
use tokio_util::sync::CancellationToken;
use zbus::{
    zvariant::{OwnedObjectPath, OwnedValue, Value},
    Connection, MatchRule, MessageStream,
};

const SERVICE: &str = "org.freedesktop.portal.Desktop";
const DESKTOP: &str = "/org/freedesktop/portal/desktop";
const REQUEST: &str = "org.freedesktop.portal.Request";
pub const MAX_FILE_BYTES: u64 = 64 * 1024 * 1024;
pub const DEFAULT_TIMEOUT: Duration = Duration::from_secs(60);
static NEXT_TOKEN: AtomicU64 = AtomicU64::new(1);

/// Stable, deliberately sanitized errors: no bus, server, URI or filesystem error text.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    Busy,
    Cancelled,
    TimedOut,
    Unavailable,
    BackendFailure,
    InvalidResponse,
    FileReadFailed,
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(match self {
            Self::Busy => "a screenshot is already active",
            Self::Cancelled => "screenshot cancelled",
            Self::TimedOut => "screenshot timed out",
            Self::Unavailable => "screenshot portal unavailable",
            Self::BackendFailure => "screenshot portal failed",
            Self::InvalidResponse => "invalid screenshot portal response",
            Self::FileReadFailed => "screenshot file missing, unreadable or oversized",
        })
    }
}
impl std::error::Error for Error {}

/// Encoded file bytes, NOT validated image data. The URI/file remains portal-owned.
#[derive(Debug)]
pub struct Screenshot {
    pub local_uri: String,
    pub bytes: Vec<u8>,
}

#[derive(Clone)]
pub struct Client {
    connection: Connection,
    busy: Arc<AtomicBool>,
}
impl Client {
    /// Connecting never requests a screenshot or authorization UI.
    pub async fn session() -> Result<Self, Error> {
        Self::session_with_timeout(DEFAULT_TIMEOUT).await
    }
    pub async fn session_with_timeout(timeout: Duration) -> Result<Self, Error> {
        let connection = tokio::time::timeout(timeout, Connection::session())
            .await
            .map_err(|_| Error::TimedOut)?
            .map_err(|_| Error::Unavailable)?;
        Ok(Self::from_connection(connection))
    }
    /// Also permits an explicitly supplied private bus for tests.
    pub fn from_connection(connection: Connection) -> Self {
        Self {
            connection,
            busy: Arc::new(AtomicBool::new(false)),
        }
    }
    /// One request per client (including clones); no queue, retry, rectangle or silent mode.
    /// `parent_window` is an exported portal window identifier, not a native pointer.
    pub fn capture(&self, parent_window: &str, timeout: Duration) -> Result<Capture, Error> {
        if self.busy.swap(true, Ordering::AcqRel) {
            return Err(Error::Busy);
        }
        let busy = Busy(self.busy.clone());
        let cancel = CancellationToken::new();
        let worker_cancel = cancel.clone();
        let connection = self.connection.clone();
        let parent = parent_window.to_owned();
        let deadline = Instant::now() + timeout;
        let (tx, rx) = oneshot::channel();
        tokio::spawn(async move {
            let _busy = busy;
            let state = Arc::new(Mutex::new(RequestState::default()));
            let guard = CloseGuard {
                connection: connection.clone(),
                state: state.clone(),
            };
            let result = tokio::select! {
                biased;
                _ = worker_cancel.cancelled() => Err(Error::Cancelled),
                result = timeout_at(deadline, perform(&connection, &parent, deadline, state)) => result.unwrap_or(Err(Error::TimedOut)),
            };
            drop(guard);
            drop(_busy);
            let _ = tx.send(result);
        });
        Ok(Capture { cancel, rx })
    }
}
struct Busy(Arc<AtomicBool>);
impl Drop for Busy {
    fn drop(&mut self) {
        self.0.store(false, Ordering::Release);
    }
}

/// Dropping this handle (including a pending `result` future) cancels the request.
pub struct Capture {
    cancel: CancellationToken,
    rx: oneshot::Receiver<Result<Screenshot, Error>>,
}
impl Capture {
    pub fn cancel(&self) {
        self.cancel.cancel();
    }
    pub async fn result(mut self) -> Result<Screenshot, Error> {
        (&mut self.rx).await.unwrap_or(Err(Error::Unavailable))
    }
}
impl Drop for Capture {
    fn drop(&mut self) {
        self.cancel.cancel();
    }
}

#[derive(Default)]
struct RequestState {
    paths: Vec<String>,
    cancelled: bool,
    responded: bool,
}
struct CloseGuard {
    connection: Connection,
    state: Arc<Mutex<RequestState>>,
}
impl Drop for CloseGuard {
    fn drop(&mut self) {
        let mut state = self.state.lock().unwrap();
        state.cancelled = true;
        if !state.responded {
            for path in &state.paths {
                close(self.connection.clone(), path.clone());
            }
        }
    }
}
fn close(connection: Connection, path: String) {
    tokio::spawn(async move {
        // Best effort; cleanup must not extend the caller's lifecycle deadline.
        let _ = tokio::time::timeout(
            Duration::from_secs(2),
            connection.call_method(Some(SERVICE), path, Some(REQUEST), "Close", &()),
        )
        .await;
    });
}

async fn perform(
    connection: &Connection,
    parent: &str,
    deadline: Instant,
    state: Arc<Mutex<RequestState>>,
) -> Result<Screenshot, Error> {
    let sender = connection
        .unique_name()
        .ok_or(Error::Unavailable)?
        .as_str()
        .trim_start_matches(':')
        .replace('.', "_");
    let token = format!(
        "deskwaifu_{}_{}",
        std::process::id(),
        NEXT_TOKEN.fetch_add(1, Ordering::Relaxed)
    );
    let expected = format!("{DESKTOP}/request/{sender}/{token}");
    state.lock().unwrap().paths.push(expected);
    // Subscribe before calling. Namespace matching also queues early responses on alternate handles.
    let rule = MatchRule::builder()
        .msg_type(zbus::message::Type::Signal)
        .sender(SERVICE)
        .map_err(|_| Error::Unavailable)?
        .interface(REQUEST)
        .map_err(|_| Error::Unavailable)?
        .member("Response")
        .map_err(|_| Error::Unavailable)?
        .path_namespace(format!("{DESKTOP}/request"))
        .map_err(|_| Error::Unavailable)?
        .build();
    let mut stream = MessageStream::for_match_rule(rule, connection, Some(64))
        .await
        .map_err(|_| Error::Unavailable)?;
    let connection_for_call = connection.clone();
    let parent = parent.to_owned();
    let call_state = state.clone();
    // Keep the method call alive on cancellation, so a late alternate handle is closed too.
    // Cleanup has a two-second grace period beyond the caller's deadline, never unbounded.
    let call = tokio::spawn(async move {
        let options = HashMap::from([
            ("handle_token", Value::from(token)),
            ("interactive", Value::from(true)),
            ("modal", Value::from(true)),
        ]);
        let reply = timeout_at(
            deadline + Duration::from_secs(2),
            connection_for_call.call_method(
                Some(SERVICE),
                DESKTOP,
                Some("org.freedesktop.portal.Screenshot"),
                "Screenshot",
                &(parent, options),
            ),
        )
        .await
        .map_err(|_| Error::TimedOut)?
        .map_err(|error| match error {
            zbus::Error::MethodError(name, _, _)
                if matches!(
                    name.as_str(),
                    "org.freedesktop.DBus.Error.ServiceUnknown"
                        | "org.freedesktop.DBus.Error.NameHasNoOwner"
                        | "org.freedesktop.DBus.Error.Disconnected"
                ) =>
            {
                Error::Unavailable
            }
            zbus::Error::InputOutput(_) => Error::Unavailable,
            _ => Error::BackendFailure,
        })?;
        let path: OwnedObjectPath = reply
            .body()
            .deserialize()
            .map_err(|_| Error::InvalidResponse)?;
        let path = path.to_string();
        if !path.starts_with(&format!("{DESKTOP}/request/")) || path.ends_with('/') {
            return Err(Error::InvalidResponse);
        }
        let mut state = call_state.lock().unwrap();
        if !state.paths.contains(&path) {
            state.paths.push(path.clone());
        }
        if state.cancelled && !state.responded {
            close(connection_for_call, path.clone());
        }
        Ok(path)
    });
    let path = call.await.map_err(|_| Error::Unavailable)??;
    while let Some(message) = stream.next().await {
        let message = message.map_err(|_| Error::Unavailable)?;
        if message.header().path().map(|p| p.as_str()) != Some(path.as_str()) {
            continue;
        }
        let (code, results): (u32, HashMap<String, OwnedValue>) = message
            .body()
            .deserialize()
            .map_err(|_| Error::InvalidResponse)?;
        state.lock().unwrap().responded = true;
        match code {
            0 => {}
            1 => return Err(Error::Cancelled),
            _ => return Err(Error::BackendFailure),
        }
        let uri = results
            .get("uri")
            .and_then(|v| <&str>::try_from(v).ok())
            .ok_or(Error::InvalidResponse)?;
        return read_local(uri).await;
    }
    Err(Error::Unavailable)
}

fn local_path(uri: &str) -> Result<PathBuf, Error> {
    // URL parsers repair malformed escapes/whitespace and normalize localhost; reject those first.
    if !uri.starts_with("file:/") || uri.bytes().any(|b| b <= 0x20 || b == 0x7f || b == b'\\') {
        return Err(Error::InvalidResponse);
    }
    let bytes = uri.as_bytes();
    for (i, b) in bytes.iter().enumerate() {
        if *b == b'%'
            && (i + 2 >= bytes.len()
                || !bytes[i + 1].is_ascii_hexdigit()
                || !bytes[i + 2].is_ascii_hexdigit())
        {
            return Err(Error::InvalidResponse);
        }
    }
    if let Some(authority) = uri.strip_prefix("file://") {
        if !authority.starts_with('/') {
            return Err(Error::InvalidResponse);
        }
    }
    let url = url::Url::parse(uri).map_err(|_| Error::InvalidResponse)?;
    if url.scheme() != "file"
        || url.host_str().is_some()
        || url.query().is_some()
        || url.fragment().is_some()
    {
        return Err(Error::InvalidResponse);
    }
    let path = url.to_file_path().map_err(|_| Error::InvalidResponse)?;
    if !path.is_absolute() || path.as_os_str().as_encoded_bytes().contains(&0) {
        return Err(Error::InvalidResponse);
    }
    Ok(path)
}
async fn read_local(uri: &str) -> Result<Screenshot, Error> {
    let path = local_path(uri)?;
    let metadata = tokio::fs::metadata(&path)
        .await
        .map_err(|_| Error::FileReadFailed)?;
    if !metadata.is_file() || metadata.len() > MAX_FILE_BYTES {
        return Err(Error::FileReadFailed);
    }
    // Avoid hanging on a FIFO substituted between metadata and open. Check the opened descriptor too.
    let file = tokio::fs::OpenOptions::new()
        .read(true)
        .custom_flags(libc::O_NONBLOCK)
        .open(path)
        .await
        .map_err(|_| Error::FileReadFailed)?;
    let metadata = file.metadata().await.map_err(|_| Error::FileReadFailed)?;
    if !metadata.is_file() || metadata.len() > MAX_FILE_BYTES {
        return Err(Error::FileReadFailed);
    }
    let mut bytes = Vec::new();
    file.take(MAX_FILE_BYTES + 1)
        .read_to_end(&mut bytes)
        .await
        .map_err(|_| Error::FileReadFailed)?;
    if bytes.len() as u64 > MAX_FILE_BYTES {
        return Err(Error::FileReadFailed);
    }
    Ok(Screenshot {
        local_uri: uri.to_owned(),
        bytes,
    })
}

#[cfg(test)]
mod tests;
