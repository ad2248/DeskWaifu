use super::*;
use std::{
    io::{BufRead, BufReader},
    process::{Child, Command, Stdio},
    sync::atomic::AtomicUsize,
};

#[test]
fn strict_local_uris() {
    for uri in [
        "https://example.org/x",
        "file://remote/a",
        "file://localhost/a",
        "file://user:pass@host/a",
        "file:///tmp/a?",
        "file:///tmp/a#",
        "file:///tmp/%",
        "file:///tmp/%xx",
        "file:///tmp/%00",
        "file:///tmp/a b",
        "file:relative",
        "file:///tmp/a\n",
        "file:///tmp/\\x",
    ] {
        assert_eq!(local_path(uri), Err(Error::InvalidResponse), "{uri:?}");
    }
    assert_eq!(
        local_path("file:///tmp/test%20%23%25.png").unwrap(),
        PathBuf::from("/tmp/test #%.png")
    );
}

#[tokio::test]
async fn bounded_files_without_decoding_or_deletion() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("截图 #%.png");
    std::fs::write(&path, b"not an image: intentionally caller-decoded").unwrap();
    let uri = url::Url::from_file_path(&path).unwrap().to_string();
    assert_eq!(
        read_local(&uri).await.unwrap().bytes,
        std::fs::read(&path).unwrap()
    );
    assert!(path.exists());
    let oversized = std::fs::File::create(&path).unwrap();
    oversized.set_len(MAX_FILE_BYTES + 1).unwrap();
    assert_eq!(read_local(&uri).await.unwrap_err(), Error::FileReadFailed);
    assert_eq!(
        read_local(url::Url::from_file_path(dir.path()).unwrap().as_ref())
            .await
            .unwrap_err(),
        Error::FileReadFailed
    );
    std::fs::remove_file(&path).unwrap();
    assert_eq!(read_local(&uri).await.unwrap_err(), Error::FileReadFailed);
}

struct PrivateBus {
    child: Child,
    address: String,
}
impl PrivateBus {
    fn new() -> Self {
        // Never read DBUS_SESSION_BUS_ADDRESS or contact the user's desktop.
        let mut child = Command::new("dbus-daemon")
            .args(["--session", "--nofork", "--print-address=1"])
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .expect("dbus-daemon required for offline mock tests");
        let mut address = String::new();
        BufReader::new(child.stdout.take().unwrap())
            .read_line(&mut address)
            .unwrap();
        Self {
            child,
            address: address.trim().to_owned(),
        }
    }
    async fn connection(&self) -> Connection {
        zbus::connection::Builder::address(self.address.as_str())
            .unwrap()
            .build()
            .await
            .unwrap()
    }
}
impl Drop for PrivateBus {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

#[derive(Clone, Copy)]
enum Mode {
    Early,
    AlternateEarly,
    NoResponse,
    Delayed,
    Cancelled,
    Rejected,
    BadUri,
    MethodError,
    BadHandle,
}
struct FakeRequest(Arc<AtomicUsize>);
#[zbus::interface(name = "org.freedesktop.portal.Request")]
impl FakeRequest {
    fn close(&self) {
        self.0.fetch_add(1, Ordering::SeqCst);
    }
}
struct FakePortal {
    mode: Mode,
    uri: String,
    calls: Arc<AtomicUsize>,
    closes: Arc<AtomicUsize>,
}
#[zbus::interface(name = "org.freedesktop.portal.Screenshot")]
impl FakePortal {
    async fn screenshot(
        &self,
        parent: &str,
        options: HashMap<String, OwnedValue>,
        #[zbus(header)] header: zbus::message::Header<'_>,
        #[zbus(connection)] connection: &Connection,
    ) -> zbus::fdo::Result<OwnedObjectPath> {
        assert_eq!(parent, "wayland:test-parent");
        assert!(bool::try_from(options.get("interactive").unwrap()).unwrap());
        let token = <&str>::try_from(options.get("handle_token").unwrap()).unwrap();
        assert!(token.starts_with("deskwaifu_"));
        self.calls.fetch_add(1, Ordering::SeqCst);
        if matches!(self.mode, Mode::MethodError) {
            return Err(zbus::fdo::Error::Failed(
                "secret credentials must not escape".into(),
            ));
        }
        if matches!(self.mode, Mode::BadHandle) {
            return Ok(OwnedObjectPath::try_from("/").unwrap());
        }
        let sender = header
            .sender()
            .unwrap()
            .as_str()
            .trim_start_matches(':')
            .replace('.', "_");
        let mut path = format!("{DESKTOP}/request/{sender}/{token}");
        if matches!(self.mode, Mode::AlternateEarly | Mode::Delayed) {
            path.push_str("_alternate");
        }
        if matches!(self.mode, Mode::Delayed) {
            tokio::time::sleep(Duration::from_millis(100)).await;
        }
        connection
            .object_server()
            .at(path.as_str(), FakeRequest(self.closes.clone()))
            .await
            .unwrap();
        if !matches!(self.mode, Mode::NoResponse | Mode::Delayed) {
            let code = match self.mode {
                Mode::Cancelled => 1u32,
                Mode::Rejected => 2,
                _ => 0,
            };
            let uri = if matches!(self.mode, Mode::BadUri) {
                "https://host.invalid/secret"
            } else {
                &self.uri
            };
            let results = HashMap::from([("uri", Value::from(uri))]);
            // Deliberately before method return; also exercise a duplicate signal.
            for _ in 0..2 {
                connection
                    .emit_signal(
                        None::<&str>,
                        path.as_str(),
                        REQUEST,
                        "Response",
                        &(code, &results),
                    )
                    .await
                    .unwrap();
            }
        }
        Ok(OwnedObjectPath::try_from(path).unwrap())
    }
}
async fn wait_count(count: &AtomicUsize, expected: usize) {
    tokio::time::timeout(Duration::from_secs(2), async {
        while count.load(Ordering::SeqCst) < expected {
            tokio::time::sleep(Duration::from_millis(2)).await;
        }
    })
    .await
    .unwrap();
}
async fn setup(
    mode: Mode,
) -> (
    PrivateBus,
    Connection,
    Client,
    tempfile::TempDir,
    Arc<AtomicUsize>,
    Arc<AtomicUsize>,
) {
    let bus = PrivateBus::new();
    let server = bus.connection().await;
    let calls = Arc::new(AtomicUsize::new(0));
    let closes = Arc::new(AtomicUsize::new(0));
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("synthetic.png");
    std::fs::write(&path, b"mock encoded image bytes").unwrap();
    server
        .object_server()
        .at(
            DESKTOP,
            FakePortal {
                mode,
                uri: url::Url::from_file_path(path).unwrap().to_string(),
                calls: calls.clone(),
                closes: closes.clone(),
            },
        )
        .await
        .unwrap();
    server.request_name(SERVICE).await.unwrap();
    let client = Client::from_connection(bus.connection().await);
    assert_eq!(
        calls.load(Ordering::SeqCst),
        0,
        "construction must not capture"
    );
    (bus, server, client, dir, calls, closes)
}

#[tokio::test]
async fn actual_private_bus_early_responses_and_sanitized_errors() {
    for mode in [
        Mode::Early,
        Mode::AlternateEarly,
        Mode::Cancelled,
        Mode::Rejected,
        Mode::BadUri,
        Mode::MethodError,
        Mode::BadHandle,
    ] {
        let (_bus, _server, client, dir, _, _) = setup(mode).await;
        let request = client
            .capture("wayland:test-parent", Duration::from_secs(2))
            .unwrap();
        assert!(matches!(
            client.clone().capture("", DEFAULT_TIMEOUT),
            Err(Error::Busy)
        ));
        let result = request.result().await;
        match mode {
            Mode::Early | Mode::AlternateEarly => {
                assert_eq!(result.unwrap().bytes, b"mock encoded image bytes")
            }
            Mode::Cancelled => assert_eq!(result.unwrap_err(), Error::Cancelled),
            Mode::Rejected | Mode::MethodError => {
                assert_eq!(result.unwrap_err(), Error::BackendFailure)
            }
            _ => assert_eq!(result.unwrap_err(), Error::InvalidResponse),
        }
        assert!(dir.path().join("synthetic.png").exists());
        assert!(!Error::BackendFailure.to_string().contains("secret"));
    }
}

#[tokio::test]
async fn cancellation_timeout_drop_and_late_alternate_handle_close() {
    for (mode, action) in [
        (Mode::NoResponse, "cancel"),
        (Mode::NoResponse, "timeout"),
        (Mode::Delayed, "cancel"),
        (Mode::Delayed, "drop"),
        (Mode::Delayed, "timeout"),
    ] {
        let (_bus, _server, client, _dir, calls, closes) = setup(mode).await;
        let request = client
            .capture(
                "wayland:test-parent",
                if action == "timeout" {
                    Duration::from_millis(40)
                } else {
                    Duration::from_secs(2)
                },
            )
            .unwrap();
        wait_count(&calls, 1).await;
        if action == "drop" {
            drop(request);
        } else {
            if action == "cancel" {
                request.cancel();
            }
            assert_eq!(
                request.result().await.unwrap_err(),
                if action == "timeout" {
                    Error::TimedOut
                } else {
                    Error::Cancelled
                }
            );
        }
        wait_count(&closes, 1).await;
    }
}
