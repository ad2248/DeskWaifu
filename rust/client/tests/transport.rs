use deskwaifu_client::{request, MAX_REQUEST, MAX_RESPONSE};
use serde_json::json;
use std::{
    io::{self, BufRead, BufReader, Write},
    os::unix::net::{UnixListener, UnixStream},
    thread,
    time::{Duration, Instant},
};

fn exchange(reply: Vec<u8>) -> io::Result<deskwaifu_client::Response> {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("ipc.sock");
    let listener = UnixListener::bind(&path).unwrap();
    let server = thread::spawn(move || {
        let (mut stream, _) = listener.accept().unwrap();
        stream
            .set_read_timeout(Some(Duration::from_secs(2)))
            .unwrap();
        stream
            .set_write_timeout(Some(Duration::from_secs(2)))
            .unwrap();
        let mut line = String::new();
        BufReader::new(stream.try_clone().unwrap())
            .read_line(&mut line)
            .unwrap();
        assert_eq!(line, "{\"command\":\"get_status\"}\n");
        // Oversized replies may be rejected before the writer finishes.
        let _ = stream.write_all(&reply);
    });
    let result = request(
        &path,
        &json!({"command": "get_status"}),
        Duration::from_secs(2),
    );
    server.join().unwrap();
    result
}

#[test]
fn success_and_server_failure_are_preserved() {
    let response = exchange(b"{\"ok\":true,\"status\":{\"visible\":false}}\n".to_vec()).unwrap();
    assert!(response.ok);
    assert_eq!(response.value["status"]["visible"], false);
    let response = exchange(b"{\"ok\":false,\"error\":\"not ready\"}\n".to_vec()).unwrap();
    assert!(!response.ok);
    assert_eq!(response.value["error"], "not ready");
}

#[test]
fn invalid_replies() {
    for reply in [
        "\n",
        "garbage\n",
        "[]\n",
        "{}\n",
        "{\"ok\":1}\n",
        "{\"ok\":true,\"ok\":false}\n",
        "{\"ok\":true,\"data\":[{\"x\":1,\"x\":2}]}\n",
        "{\"ok\":true} trailing\n",
    ] {
        assert_eq!(
            exchange(reply.as_bytes().to_vec()).unwrap_err().kind(),
            io::ErrorKind::InvalidData,
            "{reply}"
        );
    }
}

#[test]
fn truncated_replies() {
    for reply in ["", "{\"ok\":true}", "{\"ok\":"] {
        assert_eq!(
            exchange(reply.as_bytes().to_vec()).unwrap_err().kind(),
            io::ErrorKind::InvalidData
        );
    }
}

#[test]
fn response_size_boundary() {
    let prefix = b"{\"ok\":true,\"data\":\"";
    let suffix = b"\"}";
    let mut reply = prefix.to_vec();
    reply.resize(MAX_RESPONSE - suffix.len(), b'x');
    reply.extend_from_slice(suffix);
    reply.push(b'\n');
    assert!(exchange(reply.clone()).unwrap().ok);
    reply.insert(prefix.len(), b'x');
    assert_eq!(
        exchange(reply).unwrap_err().kind(),
        io::ErrorKind::InvalidData
    );
}

#[test]
fn oversized_request_is_rejected_before_connecting() {
    let error = request(
        std::path::Path::new("/does/not/exist"),
        &json!({"data": "x".repeat(MAX_REQUEST)}),
        Duration::from_secs(1),
    )
    .unwrap_err();
    assert_eq!(error.kind(), io::ErrorKind::InvalidData);
}

fn timeout_server(handler: impl FnOnce(UnixStream) + Send + 'static) -> Duration {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("ipc.sock");
    let listener = UnixListener::bind(&path).unwrap();
    let server = thread::spawn(move || handler(listener.accept().unwrap().0));
    let started = Instant::now();
    let error = request(
        &path,
        &json!({"command": "get_status"}),
        Duration::from_millis(100),
    )
    .unwrap_err();
    let elapsed = started.elapsed();
    assert!(matches!(
        error.kind(),
        io::ErrorKind::TimedOut | io::ErrorKind::WouldBlock
    ));
    server.join().unwrap();
    elapsed
}

#[test]
fn silent_peer_times_out() {
    let elapsed = timeout_server(|_stream| thread::sleep(Duration::from_millis(250)));
    assert!(elapsed < Duration::from_millis(500));
}

#[test]
fn trickling_peer_cannot_reset_deadline() {
    let elapsed = timeout_server(|mut stream| {
        for byte in b"{\"ok\":true}\n" {
            if stream.write_all(&[*byte]).is_err() {
                break;
            }
            thread::sleep(Duration::from_millis(30));
        }
    });
    assert!(elapsed < Duration::from_millis(250));
}

#[test]
fn unavailable_socket_and_zero_timeout() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("missing.sock");
    assert!(request(&path, &json!({}), Duration::from_secs(1)).is_err());
    assert_eq!(
        request(&path, &json!({}), Duration::ZERO)
            .unwrap_err()
            .kind(),
        io::ErrorKind::TimedOut
    );
}
