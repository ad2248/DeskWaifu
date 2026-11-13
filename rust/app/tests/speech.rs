//! Speech command surface: settings, validation, and the runtime's end-to-end
//! path over a loopback WebSocket server.
//!
//! The loopback server speaks the MiniMax protocol exactly as
//! `rust/speech-domain/tests/protocol.rs` does, so the runtime's real
//! `synthesize` client is exercised. With `DESKWAIFU_SPEECH_TEST_SINK=1` the
//! audio device is replaced by the runtime's silent sink, so completion can be
//! asserted without hardware.
//!
//! Nothing here reaches the real MiniMax endpoint: `--speech-endpoint` points at
//! `ws://127.0.0.1:<port>`, which the protocol layer accepts only for loopback.
//! A real cloud synthesis and a real audio device are outside this suite.

use futures_util::{SinkExt, StreamExt};
use serde_json::{json, Value};
use std::fs;
use std::io::{BufRead, BufReader, Write};
use std::net::TcpListener;
use std::os::unix::fs::PermissionsExt;
use std::os::unix::net::UnixStream;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, ExitStatus, Stdio};
use std::sync::mpsc::Sender;
use std::thread::JoinHandle;
use std::time::{Duration, Instant};
use tempfile::{tempdir, TempDir};
use tokio::net::TcpListener as TokioListener;
use tokio_tungstenite::{accept_async, tungstenite::Message, WebSocketStream};

const TIMEOUT: Duration = Duration::from_secs(10);
/// A key distinctive enough to grep every reply for it.
const SECRET: &str = "sk-loopback-never-echo-me";

struct ChildGuard(Child);

impl ChildGuard {
    fn spawn(args: &[&std::ffi::OsStr], env: &[(&str, &Path)]) -> Self {
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

impl Runtime {
    /// Starts a headless runtime with an isolated `XDG_CONFIG_HOME`, so the
    /// speech credential file belongs to the test.
    fn start(model: &Path, endpoint: Option<&str>, extra: &[(&str, &Path)]) -> Self {
        let dir = tempdir().unwrap();
        let socket = dir.path().join("runtime.sock");
        let config = dir.path().join("config");
        let mut args: Vec<&std::ffi::OsStr> = vec![
            "--model".as_ref(),
            model.as_os_str(),
            "--socket".as_ref(),
            socket.as_os_str(),
        ];
        if let Some(endpoint) = endpoint {
            args.push("--speech-endpoint".as_ref());
            args.push(std::ffi::OsStr::new(endpoint));
        }
        // CI has no audio device, so every playback test uses the silent sink by
        // default; `extra` still overrides it when a test needs a delay.
        let mut env: Vec<(&str, &Path)> = vec![
            ("XDG_CONFIG_HOME", &config),
            ("DESKWAIFU_SPEECH_TEST_SINK", Path::new("1")),
        ];
        env.extend_from_slice(extra);
        let child = ChildGuard::spawn(&args, &env);
        let runtime = Self {
            child,
            socket,
            config,
            _dir: dir,
        };
        runtime.await_socket();
        runtime
    }

    fn await_socket(&self) {
        let deadline = Instant::now() + TIMEOUT;
        loop {
            if UnixStream::connect(&self.socket).is_ok() {
                return;
            }
            assert!(Instant::now() < deadline, "runtime did not bind in time");
            std::thread::sleep(Duration::from_millis(10));
        }
    }

    /// The credential file the C++ `SpeechSettings` owns.
    fn key_file(&self) -> PathBuf {
        self.config.join("plugin/speech/config.json")
    }

    fn stored(&self) -> Option<String> {
        let text = fs::read_to_string(self.key_file()).ok()?;
        let value: Value = serde_json::from_str(&text).ok()?;
        value["api_key"].as_str().map(str::to_owned)
    }

    fn request(&self, request: Value) -> Value {
        self.request_text(&request.to_string())
    }

    fn request_text(&self, request: &str) -> Value {
        let mut stream = UnixStream::connect(&self.socket).unwrap();
        stream.set_read_timeout(Some(TIMEOUT)).unwrap();
        stream.set_write_timeout(Some(TIMEOUT)).unwrap();
        writeln!(stream, "{request}").unwrap();
        let mut response = String::new();
        BufReader::new(stream).read_line(&mut response).unwrap();
        serde_json::from_str(&response).unwrap()
    }

    /// Polls `get_speech_status` until `settled` holds.
    fn wait_status(&self, mut settled: impl FnMut(&Value) -> bool) -> Value {
        let deadline = Instant::now() + TIMEOUT;
        loop {
            let status = self.request(json!({"command":"get_speech_status"}));
            if settled(&status) {
                return status;
            }
            assert!(Instant::now() < deadline, "speech status stuck at {status}");
            std::thread::sleep(Duration::from_millis(10));
        }
    }

    fn configure(&self) {
        assert_eq!(
            self.request(json!({"command":"set_speech_settings","api_key":SECRET}))["ok"],
            true
        );
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

/// What the peer does with the connection the runtime opens.
enum Peer {
    /// Completes one synthesis of `frames` mono PCM16 frames at 24 kHz.
    Synthesize(usize),
    /// Accepts the handshake and then hangs, so the playback stays in
    /// `Preparing` until the runtime cancels it.
    Hold,
    /// Starts the silent sink, then keeps synthesis open until the client
    /// cancels it after a local audio failure. Reports that observed close.
    AudioFailure(Sender<()>),
    /// Reports the MiniMax authentication rejection without a handshake message.
    Reject,
}

/// A loopback MiniMax-protocol peer.
///
/// The listener is bound before the runtime starts and one current-thread
/// runtime serves every expected connection in order, so nothing outlives its
/// reactor. A later connection waits in the kernel backlog until the earlier
/// peer is done, which is exactly the ordering the runtime produces when it
/// replaces one speech with the next.
struct Server {
    endpoint: String,
    peers: Option<Sender<Peer>>,
    thread: Option<JoinHandle<()>>,
}

impl Server {
    /// Binds the listener, then serves peers as they are queued.
    fn start() -> Self {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        listener.set_nonblocking(true).unwrap();
        let endpoint = format!("ws://{}", listener.local_addr().unwrap());
        let (peers, incoming) = std::sync::mpsc::channel();
        let thread = std::thread::spawn(move || {
            tokio::runtime::Builder::new_current_thread()
                .enable_all()
                .build()
                .unwrap()
                .block_on(async move {
                    let listener = TokioListener::from_std(listener).unwrap();
                    while let Ok(peer) = incoming.recv() {
                        let (stream, _) = listener.accept().await.expect("peer accepted");
                        let ws = accept_async(stream).await.expect("peer handshake");
                        serve_peer(ws, peer).await;
                    }
                });
        });
        Self {
            endpoint,
            peers: Some(peers),
            thread: Some(thread),
        }
    }

    /// Queues the reply for the next connection the runtime opens.
    fn expect(&mut self, peer: Peer) {
        self.peers.as_ref().unwrap().send(peer).unwrap();
    }

    /// Stops accepting and waits for every queued peer to finish.
    fn join(mut self) {
        self.peers = None;
        if let Some(thread) = self.thread.take() {
            thread.join().unwrap();
        }
    }
}

async fn serve_peer(mut ws: WebSocketStream<tokio::net::TcpStream>, peer: Peer) {
    // Every peer except `Reject` starts with the protocol's connect event.
    if !matches!(peer, Peer::Reject) {
        send(&mut ws, "connected_success", json!({})).await;
        assert_eq!(incoming(&mut ws).await["event"], "task_start");
    }
    let mut closed = None;
    match peer {
        Peer::Synthesize(frames) => {
            send(&mut ws, "task_started", json!({})).await;
            assert_eq!(incoming(&mut ws).await["event"], "task_continue");
            assert_eq!(incoming(&mut ws).await["event"], "task_finish");
            send(&mut ws, "sentence_start", json!({})).await;
            // Two bytes per mono frame, hex encoded as the protocol demands.
            let audio = "00".repeat(frames * 2);
            send(&mut ws, "task_continued", json!({"data":{"audio":audio}})).await;
            send(&mut ws, "sentence_end", json!({})).await;
            send(&mut ws, "task_finished", json!({})).await;
        }
        Peer::Reject => {
            ws.send(Message::Text(
                json!({"event":"error","base_resp":{"status_code":1004,"status_msg":"secret"}})
                    .to_string(),
            ))
            .await
            .unwrap();
        }
        Peer::Hold => {}
        Peer::AudioFailure(notify) => {
            send(&mut ws, "task_started", json!({})).await;
            // No task_finished, error, or server close: only the runtime's local
            // audio failure can end this connection before its 30 s timeout.
            closed = Some(notify);
        }
    }
    // Hold the socket until the runtime drops it, so a cancelled playback's
    // socket really closes. A `Synthesize` peer ends on the runtime's own close.
    let wait_for_close = async {
        while let Some(message) = ws.next().await {
            match message {
                Ok(Message::Close(_)) | Err(_) => break,
                Ok(_) => continue,
            }
        }
    };
    if let Some(notify) = closed {
        tokio::time::timeout(TIMEOUT, wait_for_close)
            .await
            .expect("local audio failure did not close the synthesis connection");
        notify.send(()).unwrap();
    } else {
        wait_for_close.await;
    }
}

async fn incoming(ws: &mut WebSocketStream<tokio::net::TcpStream>) -> Value {
    let message = ws.next().await.unwrap().unwrap();
    serde_json::from_str(message.to_text().unwrap()).unwrap()
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
// Settings
// ---------------------------------------------------------------------------

#[test]
fn settings_are_unconfigured_reported_and_never_echoed() {
    let Some(model) = fixture() else { return };
    let runtime = Runtime::start(&model, None, &[]);
    let first = runtime.request(json!({"command":"get_speech_settings"}));
    assert_eq!(first, json!({"ok":true,"configured":false,"error":""}));
    assert!(
        !runtime.key_file().exists(),
        "an absent read creates nothing"
    );

    let set = runtime.request(json!({"command":"set_speech_settings","api_key":SECRET}));
    assert_eq!(set, json!({"ok":true,"configured":true,"error":""}));
    assert_eq!(runtime.stored().as_deref(), Some(SECRET));

    let read = runtime.request(json!({"command":"get_speech_settings"}));
    assert_eq!(read, json!({"ok":true,"configured":true,"error":""}));
    let status = runtime.request(json!({"command":"get_speech_status"}));
    assert_eq!(status["configured"], true);
    // The key must not appear in any reply; a careless implementation would
    // reflect the request, the stored document or the service response.
    for reply in [&first, &set, &read, &status] {
        assert!(!reply.to_string().contains(SECRET), "{reply}");
    }

    let mode = fs::metadata(runtime.key_file())
        .unwrap()
        .permissions()
        .mode()
        & 0o777;
    assert_eq!(mode, 0o600);

    // The empty string clears the credential, keeping the file and its mode.
    let cleared = runtime.request(json!({"command":"set_speech_settings","api_key":""}));
    assert_eq!(cleared, json!({"ok":true,"configured":false,"error":""}));
    assert_eq!(runtime.stored(), None);
    assert_eq!(
        runtime.request(json!({"command":"get_speech_settings"}))["configured"],
        false
    );
    assert_eq!(
        fs::metadata(runtime.key_file())
            .unwrap()
            .permissions()
            .mode()
            & 0o777,
        0o600
    );
    runtime.quit();
}

#[test]
fn malformed_settings_requests_fail_and_leave_the_key_in_place() {
    let Some(model) = fixture() else { return };
    let runtime = Runtime::start(&model, None, &[]);
    runtime.configure();
    // Everything the C++ `SpeechSettings::Set` rejects: a missing field, a
    // non-string, an over-long value and an ASCII control character.
    for request in [
        json!({"command":"set_speech_settings"}),
        json!({"command":"set_speech_settings","api_key":17}),
        json!({"command":"set_speech_settings","api_key":"x".repeat(4097)}),
        json!({"command":"set_speech_settings","api_key":"bad\nkey"}),
    ] {
        let reply = runtime.request(request.clone());
        assert_eq!(reply["ok"], false, "{request}");
        assert_eq!(reply["configured"], true, "{request}");
        assert_eq!(reply["error"], "Unable to read or update speech settings");
        assert!(!reply.to_string().contains(SECRET));
        assert_eq!(runtime.stored().as_deref(), Some(SECRET), "{request}");
    }
    // A duplicated `api_key` is rejected before it can be collapsed: the C++
    // `SpeechSettings::Set` parses the raw document first for exactly this case.
    let duplicate =
        format!(r#"{{"command":"set_speech_settings","api_key":"{SECRET}","api_key":"other"}}"#);
    let reply = runtime.request_text(&duplicate);
    assert_eq!(reply["ok"], false);
    assert_eq!(reply["error"], "Unable to read or update speech settings");
    assert_eq!(runtime.stored().as_deref(), Some(SECRET));
    runtime.quit();
}

// ---------------------------------------------------------------------------
// Speak validation
// ---------------------------------------------------------------------------

#[test]
fn blank_text_fails_without_disturbing_the_active_speech() {
    let Some(model) = fixture() else { return };
    let mut server = Server::start();
    server.expect(Peer::Hold);
    let runtime = Runtime::start(&model, Some(&server.endpoint), &[]);
    runtime.configure();
    assert_eq!(
        runtime.request(json!({"command":"speak","text":"hello"}))["ok"],
        true
    );
    let active = runtime.wait_status(|status| status["state"] == "preparing");
    assert_eq!(active["active"], true);
    let id = active["id"].as_u64().unwrap_or(0);

    for text in ["", "   ", "\n\t "] {
        let reply = runtime.request(json!({"command":"speak","text":text,"replace":true}));
        assert_eq!(reply["ok"], false, "{text:?}");
        assert_eq!(reply["error"], "speech text is empty");
    }
    // The capability also requires the field types, and rejects both like the
    // C++ `SpeechPlugin::Handle`.
    assert_eq!(
        runtime.request(json!({"command":"speak","replace":true}))["error"],
        "Invalid speech request"
    );
    assert_eq!(
        runtime.request(json!({"command":"speak","text":"hi","replace":"yes"}))["error"],
        "Invalid speech request"
    );

    // None of those touched the playback: same id, still active, not idle.
    let still = runtime.request(json!({"command":"get_speech_status"}));
    assert_eq!(still["active"], true);
    assert_eq!(still["id"], id);
    assert_eq!(still["state"], "preparing");
    assert_eq!(still["last_error"], Value::Null);
    runtime.quit();
    server.join();
}

#[test]
fn text_length_uses_utf16_code_units_like_qstring() {
    let Some(model) = fixture() else { return };
    let mut server = Server::start();
    // 9999 units is inside the C++ bound: accepted, and it really connects.
    server.expect(Peer::Synthesize(8));
    let runtime = Runtime::start(&model, Some(&server.endpoint), &[]);
    runtime.configure();
    let boundary = "y".repeat(9_999);
    assert_eq!(
        runtime.request(json!({"command":"speak","text":boundary}))["ok"],
        true
    );
    let done = runtime.wait_status(|status| status["active"] == false);
    assert_eq!(done["state"], "idle");
    assert_eq!(done["last_error"], Value::Null);

    // 10000 fails, and so does 5000 astral characters: 10000 UTF-16 units is
    // over the bound even though it is only 5000 `char`s.
    let over = "y".repeat(10_000);
    assert_eq!(
        runtime.request(json!({"command":"speak","text":over}))["error"],
        "无效的语音文本或参数"
    );
    let pairs = "😀".repeat(5_000);
    assert_eq!(pairs.chars().count(), 5_000);
    assert_eq!(
        runtime.request(json!({"command":"speak","text":pairs}))["error"],
        "无效的语音文本或参数"
    );
    assert_eq!(
        runtime.request(json!({"command":"get_speech_status"}))["last_error"],
        Value::Null,
        "a rejected length is not a playback failure"
    );
    runtime.quit();
    server.join();
}

#[test]
fn busy_without_replace_fails_and_replace_cancels_to_start_the_newest() {
    let Some(model) = fixture() else { return };
    let mut server = Server::start();
    server.expect(Peer::Hold);
    server.expect(Peer::Synthesize(8));
    let runtime = Runtime::start(&model, Some(&server.endpoint), &[]);
    runtime.configure();
    assert_eq!(
        runtime.request(json!({"command":"speak","text":"first"}))["ok"],
        true
    );
    let first = runtime.wait_status(|status| status["state"] == "preparing");
    let first_id = first["id"].as_u64().unwrap();

    let busy = runtime.request(json!({"command":"speak","text":"second"}));
    assert_eq!(busy["ok"], false);
    assert_eq!(busy["error"], "speech service is busy");
    assert_eq!(
        runtime.request(json!({"command":"get_speech_status"}))["id"],
        first_id
    );

    // `replace` cancels the held playback and starts the newest.
    assert_eq!(
        runtime.request(json!({"command":"speak","text":"second","replace":true}))["ok"],
        true
    );
    let second = runtime.wait_status(|status| status["id"] != first_id);
    assert!(second["id"].as_u64().unwrap() > first_id);
    let done = runtime.wait_status(|status| status["active"] == false);
    assert_eq!(done["state"], "idle");
    assert_eq!(done["last_error"], Value::Null);
    runtime.quit();
    server.join();
}

#[test]
fn an_unconfigured_key_fails_before_any_connection() {
    let Some(model) = fixture() else { return };
    // No endpoint is needed: the failure happens before the socket opens.
    let runtime = Runtime::start(&model, Some("ws://127.0.0.1:9/unused"), &[]);
    let reply = runtime.request(json!({"command":"speak","text":"hello"}));
    assert_eq!(reply["ok"], false);
    assert_eq!(reply["error"], "请先在控制面板中设置 MiniMax API Key");
    let status = runtime.request(json!({"command":"get_speech_status"}));
    assert_eq!(status["active"], false);
    assert_eq!(status["state"], "idle");
    assert_eq!(status["configured"], false);
    assert_eq!(status["last_error"], Value::Null);
    runtime.quit();
}

#[test]
fn a_service_rejection_is_reported_and_leaves_the_machine_idle() {
    let Some(model) = fixture() else { return };
    let mut server = Server::start();
    server.expect(Peer::Reject);
    let runtime = Runtime::start(&model, Some(&server.endpoint), &[]);
    runtime.configure();
    assert_eq!(
        runtime.request(json!({"command":"speak","text":"hello"}))["ok"],
        true
    );
    let failed = runtime.wait_status(|status| status["last_error"].is_string());
    assert_eq!(failed["active"], false);
    assert_eq!(failed["state"], "idle");
    assert_eq!(failed["last_error"], "语音服务拒绝请求（代码 1004）");
    assert!(!failed.to_string().contains(SECRET));
    // The runtime recovers: a later request is accepted, not stuck on the error.
    assert_eq!(
        runtime.request(json!({"command":"get_speech_status"}))["configured"],
        true
    );
    runtime.quit();
    server.join();
}

#[test]
fn a_local_audio_failure_closes_the_unfinished_synthesis_connection() {
    let Some(model) = fixture() else { return };
    let mut server = Server::start();
    let (closed, observed_close) = std::sync::mpsc::channel();
    server.expect(Peer::AudioFailure(closed));
    let runtime = Runtime::start(
        &model,
        Some(&server.endpoint),
        &[("DESKWAIFU_SPEECH_TEST_SINK_FAIL", Path::new("1"))],
    );
    runtime.configure();
    assert_eq!(
        runtime.request(json!({"command":"speak","text":"hello"}))["ok"],
        true
    );
    let failed = runtime.wait_status(|status| status["last_error"].is_string());
    assert_eq!(failed["last_error"], "音频输出设备发生错误");
    assert_eq!(failed["active"], false);
    assert_eq!(failed["state"], "idle");
    // Checking status alone misses the leak: the machine was already idle even
    // when its source receiver kept the WebSocket task alive. Require the peer
    // to observe closure before quit/replacement can cancel that task for us.
    observed_close
        .recv_timeout(TIMEOUT)
        .expect("idle playback retained its unfinished synthesis connection");
    assert_eq!(
        runtime.request(json!({"command":"get_speech_status"})),
        failed,
        "the runtime remains alive with the same failed playback status"
    );
    runtime.quit();
    server.join();
}

// ---------------------------------------------------------------------------
// End-to-end: completion waits for the audio to drain
// ---------------------------------------------------------------------------

#[test]
fn completion_is_reported_only_after_the_audio_drained() {
    let Some(model) = fixture() else { return };
    let mut server = Server::start();
    // 1.5 s of audio at 24 kHz, pulled every 40 ms, so `draining` is visible.
    server.expect(Peer::Synthesize(36_000));
    let runtime = Runtime::start(
        &model,
        Some(&server.endpoint),
        &[
            ("DESKWAIFU_SPEECH_TEST_SINK", Path::new("1")),
            ("DESKWAIFU_SPEECH_TEST_SINK_DELAY_MS", Path::new("40")),
        ],
    );
    runtime.configure();
    assert_eq!(
        runtime.request(json!({"command":"speak","text":"a long sentence"}))["ok"],
        true
    );
    // The source is finished long before the device is: the state reaches
    // `draining` while the playback is still active, which is the SP-04 gap this
    // runtime must reproduce instead of reporting completion at source EOF.
    let draining = runtime.wait_status(|status| status["state"] == "draining");
    assert_eq!(draining["active"], true);
    assert_eq!(draining["last_error"], Value::Null);
    let done = runtime.wait_status(|status| status["active"] == false);
    assert_eq!(done["state"], "idle");
    assert_eq!(done["id"], draining["id"]);
    assert_eq!(done["last_error"], Value::Null);
    runtime.quit();
    server.join();
}
