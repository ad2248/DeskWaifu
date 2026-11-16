#![cfg(unix)]

use deskwaifu_os::{MAX_FRAME_BYTES, Request, Response, Server};
use deskwaifu_runtime::Runtime;
use deskwaifu_test_plugin::TestPlugin;
use std::{
    collections::VecDeque,
    fs,
    io::{self, Read, Write},
    net::Shutdown,
    os::unix::{
        fs::{FileTypeExt, MetadataExt, PermissionsExt},
        net::{UnixListener, UnixStream},
    },
    path::PathBuf,
    sync::atomic::{AtomicUsize, Ordering},
    time::{Duration, Instant},
};

// Each test owns its directory, including during unwinding. Keep Unix socket paths short.
struct Directory(PathBuf);
impl Directory {
    fn new() -> Self {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        fs::create_dir_all("/tmp/opencode").unwrap();
        loop {
            let path = PathBuf::from(format!(
                "/tmp/opencode/os-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            ));
            match fs::create_dir(&path) {
                Ok(()) => return Self(path),
                Err(error) if error.kind() == io::ErrorKind::AlreadyExists => continue,
                Err(error) => panic!("create test directory: {error}"),
            }
        }
    }

    fn socket(&self) -> PathBuf {
        self.0.join("ipc.sock")
    }
}
impl Drop for Directory {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

// Nonblocking client I/O is driven alongside Server::poll, so large responses cannot
// deadlock waiting for the test to read, and no sleeps or background server are needed.
struct Peer {
    stream: UnixStream,
    pending: VecDeque<u8>,
    input: Vec<u8>,
    frames: VecDeque<Vec<u8>>,
    eof: bool,
}
impl Peer {
    fn connect(directory: &Directory) -> Self {
        let stream = UnixStream::connect(directory.socket()).unwrap();
        stream.set_nonblocking(true).unwrap();
        Self {
            stream,
            pending: VecDeque::new(),
            input: Vec::new(),
            frames: VecDeque::new(),
            eof: false,
        }
    }

    fn send(&mut self, bytes: &[u8]) {
        self.pending.extend(bytes);
    }

    fn request(&mut self, args: &[&str]) {
        self.send(&request(args));
    }

    fn step(&mut self) {
        if !self.pending.is_empty() {
            match self.stream.write(self.pending.make_contiguous()) {
                Ok(0) => panic!("client write unexpectedly returned zero"),
                Ok(n) => {
                    self.pending.drain(..n);
                }
                Err(error)
                    if matches!(
                        error.kind(),
                        io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
                    ) => {}
                Err(error) => panic!("client write: {error}"),
            }
        }
        let mut bytes = [0; 8192];
        loop {
            match self.stream.read(&mut bytes) {
                Ok(0) => {
                    self.eof = true;
                    break;
                }
                Ok(n) => self.input.extend_from_slice(&bytes[..n]),
                Err(error) if error.kind() == io::ErrorKind::WouldBlock => break,
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(error) => panic!("client read: {error}"),
            }
        }
        while let Some(end) = self.input.iter().position(|byte| *byte == b'\n') {
            let frame: Vec<_> = self.input.drain(..=end).collect();
            assert!(
                frame.len() <= MAX_FRAME_BYTES + 1,
                "oversized wire response"
            );
            self.frames.push_back(frame);
        }
    }

    fn receive(
        &mut self,
        server: &mut Server,
        handler: &mut impl FnMut(&[String]) -> Result<String, String>,
    ) -> Response {
        self.until(server, handler, |peer| !peer.frames.is_empty());
        serde_json::from_slice(&self.frames.pop_front().unwrap()).unwrap()
    }

    fn until(
        &mut self,
        server: &mut Server,
        handler: &mut impl FnMut(&[String]) -> Result<String, String>,
        done: impl Fn(&Self) -> bool,
    ) {
        let deadline = Instant::now() + Duration::from_secs(3);
        while !done(self) {
            assert!(Instant::now() < deadline, "IPC progress deadline exceeded");
            self.step();
            server.poll(&mut *handler).unwrap();
            self.step();
            std::thread::yield_now();
        }
    }
}

fn request(args: &[&str]) -> Vec<u8> {
    let mut bytes =
        serde_json::to_vec(&Request::cli(args.iter().map(|s| (*s).into()).collect())).unwrap();
    bytes.push(b'\n');
    bytes
}

fn error_contains(response: Response, expected: &str) {
    match response {
        Response::Error { error } => assert!(
            error.contains(expected),
            "{error:?} did not contain {expected:?}"
        ),
        response => panic!("expected error containing {expected:?}, got {response:?}"),
    }
}

fn ok(output: &str) -> Response {
    Response::Ok {
        output: output.into(),
    }
}

#[test]
fn fragmented_and_coalesced_frames_keep_connection_and_argument_boundaries() {
    let directory = Directory::new();
    let mut server = Server::bind(directory.socket()).unwrap();
    let mut peer = Peer::connect(&directory);
    let args = [
        "test",
        "中文 参数",
        "",
        "line one\nline two",
        "  spaces  ",
        "001",
    ];
    let frame = request(&args);
    let mut seen = Vec::new();
    let mut handler = |args: &[String]| {
        seen.push(args.to_vec());
        Ok(serde_json::to_string(args).unwrap())
    };
    // Split inside a UTF-8 codepoint, then withhold the terminating LF.
    let split = frame.iter().position(|byte| *byte >= 0x80).unwrap() + 1;
    for fragment in [&frame[..split], &frame[split..frame.len() - 1]] {
        peer.send(fragment);
        peer.until(&mut server, &mut handler, |peer| peer.pending.is_empty());
        for _ in 0..8 {
            server.poll(&mut handler).unwrap();
            peer.step();
        }
        assert!(peer.frames.is_empty(), "partial frame must not dispatch");
    }
    let mut tail = vec![b'\n'];
    tail.extend(request(&["second"]));
    tail.extend(request(&["third", "a b"]));
    peer.send(&tail);
    assert_eq!(
        peer.receive(&mut server, &mut handler),
        ok(&serde_json::to_string(&args).unwrap())
    );
    assert_eq!(peer.receive(&mut server, &mut handler), ok("[\"second\"]"));
    assert_eq!(
        peer.receive(&mut server, &mut handler),
        ok("[\"third\",\"a b\"]")
    );
    peer.request(&["fourth"]);
    assert_eq!(peer.receive(&mut server, &mut handler), ok("[\"fourth\"]"));
    assert_eq!(
        seen,
        vec![
            args.iter().map(|s| s.to_string()).collect::<Vec<_>>(),
            vec!["second".into()],
            vec!["third".into(), "a b".into()],
            vec!["fourth".into()]
        ]
    );
}

#[test]
fn help_test_and_unknown_command_run_through_real_runtime() {
    let directory = Directory::new();
    let mut server = Server::bind(directory.socket()).unwrap();
    let mut runtime = Runtime::new(16);
    runtime.add(TestPlugin::new()).unwrap();
    runtime.start().unwrap();
    let mut handler = |args: &[String]| runtime.dispatch_command(args);
    let mut peer = Peer::connect(&directory);
    peer.request(&["help"]);
    let Response::Ok { output } = peer.receive(&mut server, &mut handler) else {
        panic!("help failed")
    };
    assert!(output.contains("help  Show available commands"));
    assert!(output.contains("test  Run the test command."));
    assert!(output.contains("All following input is passed through as string arguments."));
    peer.request(&["no-such-command"]);
    peer.request(&["test", "中文 空格", "", "a\nb"]);
    error_contains(
        peer.receive(&mut server, &mut handler),
        "unknown command: no-such-command",
    );
    assert_eq!(
        peer.receive(&mut server, &mut handler),
        ok("test command received 3 argument(s): [\"中文 空格\", \"\", \"a\\nb\"]")
    );
    peer.request(&["help"]);
    assert_eq!(peer.receive(&mut server, &mut handler), ok(&output));
}

#[test]
fn malformed_envelopes_and_unsupported_sources_never_dispatch() {
    let directory = Directory::new();
    let mut server = Server::bind(directory.socket()).unwrap();
    let mut peer = Peer::connect(&directory);
    let mut calls = 0;
    let mut handler = |_: &[String]| {
        calls += 1;
        Ok("valid".into())
    };
    let invalid: &[&[u8]] = &[
        b"not json\n",
        b"{\"source\":\"cli\",\"payload\":[\"\xff\"]}\n",
        b"{\"source\":\"cli\",\"payload\":\"test\"}\n",
        b"{\"source\":\"cli\",\"payload\":[1]}\n",
        b"{\"source\":\"cli\",\"payload\":null}\n",
        b"{\"source\":\"cli\"}\n",
        b"{\"payload\":[]}\n",
        b"{\"source\":\"cli\",\"payload\":[],\"extra\":true}\n",
        b"\n",
    ];
    for frame in invalid {
        peer.send(frame);
        error_contains(peer.receive(&mut server, &mut handler), "invalid request:");
    }
    for source in ["unknown", "gui", "CLI", ""] {
        peer.send(format!("{{\"source\":{source:?},\"payload\":[\"test\"]}}\n").as_bytes());
        error_contains(
            peer.receive(&mut server, &mut handler),
            "unsupported source",
        );
    }
    peer.request(&["valid"]);
    assert_eq!(peer.receive(&mut server, &mut handler), ok("valid"));
    assert_eq!(
        calls, 1,
        "invalid requests must not reach the command callback"
    );
}

#[test]
fn request_at_limit_succeeds_but_oversized_request_closes_only_its_connection() {
    let directory = Directory::new();
    let mut server = Server::bind(directory.socket()).unwrap();
    let mut peer = Peer::connect(&directory);
    let overhead = request(&[""]).len() - 1;
    let argument = "x".repeat(MAX_FRAME_BYTES - overhead);
    let frame = request(&[&argument]);
    assert_eq!(frame.len(), MAX_FRAME_BYTES + 1);
    let mut seen = Vec::new();
    let mut handler = |args: &[String]| {
        seen.push(args[0].len());
        Ok("accepted".into())
    };
    peer.send(&frame);
    assert_eq!(peer.receive(&mut server, &mut handler), ok("accepted"));
    let mut oversized = Peer::connect(&directory);
    // No LF: rejecting oversized input must not wait indefinitely for a delimiter.
    oversized.send(&vec![b'x'; MAX_FRAME_BYTES + 1]);
    error_contains(
        oversized.receive(&mut server, &mut handler),
        "request exceeds 64 KiB",
    );
    oversized.until(&mut server, &mut handler, |peer| peer.eof);
    assert!(oversized.frames.is_empty());
    peer.request(&["still alive"]);
    assert_eq!(peer.receive(&mut server, &mut handler), ok("accepted"));
    assert_eq!(seen, vec![argument.len(), "still alive".len()]);
}

#[test]
fn large_responses_are_fully_written_and_oversized_responses_are_bounded() {
    let directory = Directory::new();
    let mut server = Server::bind(directory.socket()).unwrap();
    let mut peer = Peer::connect(&directory);
    let overhead = serde_json::to_vec(&ok("")).unwrap().len();
    let boundary = "x".repeat(MAX_FRAME_BYTES - overhead);
    let mut handler = |args: &[String]| match args[0].as_str() {
        "boundary" => Ok(boundary.clone()),
        "oversized" => Ok("x".repeat(MAX_FRAME_BYTES)),
        // JSON escaping, not just the Rust string's length, counts toward the limit.
        "escaped" => Ok("\n".repeat(MAX_FRAME_BYTES / 2)),
        "error" => Err("x".repeat(MAX_FRAME_BYTES)),
        _ => Ok("recovered".into()),
    };
    peer.request(&["boundary"]);
    assert_eq!(peer.receive(&mut server, &mut handler), ok(&boundary));
    for command in ["oversized", "escaped", "error"] {
        peer.request(&[command]);
        error_contains(
            peer.receive(&mut server, &mut handler),
            "response exceeds 64 KiB",
        );
    }
    peer.request(&["next"]);
    assert_eq!(peer.receive(&mut server, &mut handler), ok("recovered"));
}

#[test]
fn unread_responses_do_not_block_a_second_client() {
    let directory = Directory::new();
    let mut server = Server::bind(directory.socket()).unwrap();
    let mut unread = UnixStream::connect(directory.socket()).unwrap();
    unread
        .set_write_timeout(Some(Duration::from_secs(3)))
        .unwrap();
    const REQUESTS: usize = 128;
    // A few KiB of requests generate almost 8 MiB of responses. Never read from
    // this socket: it must remain backpressured while the healthy client runs.
    unread
        .write_all(&request(&["large"]).repeat(REQUESTS))
        .unwrap();
    let output = "x".repeat(MAX_FRAME_BYTES - serde_json::to_vec(&ok("")).unwrap().len());
    let large_calls = std::cell::Cell::new(0);
    let mut handler = |args: &[String]| {
        if args[0] == "large" {
            large_calls.set(large_calls.get() + 1);
            Ok(output.clone())
        } else {
            Ok("healthy".into())
        }
    };
    let deadline = Instant::now() + Duration::from_secs(3);
    for _ in 0..4096 {
        assert!(Instant::now() < deadline, "unread client stalled polling");
        server.poll(&mut handler).unwrap();
    }
    assert!(large_calls.get() > 0);
    assert!(
        large_calls.get() < REQUESTS,
        "expected unread responses to exert backpressure"
    );
    let stalled_at = large_calls.get();
    let mut healthy = Peer::connect(&directory);
    for _ in 0..3 {
        healthy.request(&["small"]);
        assert_eq!(healthy.receive(&mut server, &mut handler), ok("healthy"));
    }
    assert_eq!(
        large_calls.get(),
        stalled_at,
        "first client must remain stalled while second client progresses"
    );
    // Dropping the backpressured client must also leave the healthy one usable.
    drop(unread);
    healthy.request(&["after disconnect"]);
    assert_eq!(healthy.receive(&mut server, &mut handler), ok("healthy"));
}

#[test]
fn eof_with_partial_frame_reports_error_without_dispatch() {
    let directory = Directory::new();
    let mut server = Server::bind(directory.socket()).unwrap();
    let mut peer = Peer::connect(&directory);
    let mut calls = 0;
    let mut handler = |_: &[String]| {
        calls += 1;
        Ok("complete".into())
    };
    peer.request(&["complete"]);
    let partial = request(&["missing LF"]);
    peer.send(&partial[..partial.len() - 1]);
    peer.until(&mut server, &mut handler, |peer| peer.pending.is_empty());
    peer.stream.shutdown(Shutdown::Write).unwrap();
    assert_eq!(peer.receive(&mut server, &mut handler), ok("complete"));
    error_contains(
        peer.receive(&mut server, &mut handler),
        "incomplete request",
    );
    peer.until(&mut server, &mut handler, |peer| peer.eof);
    assert_eq!(calls, 1);
}

#[test]
fn empty_eof_and_disconnected_clients_do_not_break_other_connections() {
    let directory = Directory::new();
    let mut server = Server::bind(directory.socket()).unwrap();
    let mut handler = |_: &[String]| Ok("x".repeat(32 * 1024));
    let mut empty = Peer::connect(&directory);
    empty.stream.shutdown(Shutdown::Write).unwrap();
    empty.until(&mut server, &mut handler, |peer| peer.eof);
    assert!(empty.frames.is_empty());
    assert!(empty.input.is_empty());
    // Disconnect with a request already accepted and a multi-poll response pending.
    let mut broken = Peer::connect(&directory);
    broken.request(&["large"]);
    broken.until(&mut server, &mut handler, |peer| peer.pending.is_empty());
    drop(broken);
    // Also disconnect with an incomplete input buffer.
    let mut partial = Peer::connect(&directory);
    partial.send(b"{\"source\":");
    partial.until(&mut server, &mut handler, |peer| peer.pending.is_empty());
    drop(partial);
    let mut healthy = Peer::connect(&directory);
    healthy.request(&["works"]);
    assert_eq!(
        healthy.receive(&mut server, &mut handler),
        ok(&"x".repeat(32 * 1024))
    );
    healthy.request(&["still works"]);
    assert_eq!(
        healthy.receive(&mut server, &mut handler),
        ok(&"x".repeat(32 * 1024))
    );
}

#[test]
fn bind_preserves_existing_regular_file_and_live_or_stale_socket() {
    let directory = Directory::new();
    let path = directory.socket();
    fs::write(&path, b"do not overwrite").unwrap();
    let original = fs::symlink_metadata(&path).unwrap();
    assert!(Server::bind(&path).is_err());
    assert_eq!(fs::read(&path).unwrap(), b"do not overwrite");
    assert_eq!(fs::symlink_metadata(&path).unwrap().ino(), original.ino());
    fs::remove_file(&path).unwrap();
    let listener = UnixListener::bind(&path).unwrap();
    let inode = fs::symlink_metadata(&path).unwrap().ino();
    assert!(Server::bind(&path).is_err());
    assert_eq!(fs::symlink_metadata(&path).unwrap().ino(), inode);
    let client = UnixStream::connect(&path).unwrap();
    let _accepted = listener.accept().unwrap();
    drop(client);
    drop(listener);
    assert!(
        Server::bind(&path).is_err(),
        "stale socket must not be removed either"
    );
    assert_eq!(fs::symlink_metadata(&path).unwrap().ino(), inode);
}

#[test]
fn socket_has_private_permissions_and_drop_removes_owned_path() {
    let directory = Directory::new();
    let path = directory.socket();
    let server = Server::bind(&path).unwrap();
    let metadata = fs::symlink_metadata(&path).unwrap();
    assert!(metadata.file_type().is_socket());
    assert_eq!(metadata.permissions().mode() & 0o777, 0o600);
    drop(server);
    assert!(!path.exists());
    // A fresh bind proves cleanup did not leave an unusable stale socket.
    drop(Server::bind(&path).unwrap());
    assert!(!path.exists());
}

#[test]
fn drop_does_not_remove_replacement_file_socket_or_symlink() {
    for replacement in ["file", "socket", "symlink"] {
        let directory = Directory::new();
        let path = directory.socket();
        let server = Server::bind(&path).unwrap();
        // Rename keeps the original inode allocated and prevents inode reuse.
        let original = directory.0.join("original.sock");
        fs::rename(&path, &original).unwrap();
        let mut listener = None;
        match replacement {
            "file" => fs::write(&path, b"replacement").unwrap(),
            "socket" => listener = Some(UnixListener::bind(&path).unwrap()),
            "symlink" => std::os::unix::fs::symlink(&original, &path).unwrap(),
            _ => unreachable!(),
        }
        let before = fs::symlink_metadata(&path).unwrap();
        drop(server);
        let after = fs::symlink_metadata(&path).expect("replacement must survive server drop");
        assert_eq!((after.dev(), after.ino()), (before.dev(), before.ino()));
        if replacement == "file" {
            assert_eq!(fs::read(&path).unwrap(), b"replacement");
        }
        if let Some(listener) = listener {
            let _client = UnixStream::connect(&path).unwrap();
            let _accepted = listener.accept().unwrap();
        }
    }
}
