use deskwaifu_ipc::{IpcServer, MAX_REQUEST, MAX_RESPONSE};
use std::fs;
use std::io::{Read, Write};
use std::os::unix::net::UnixStream as ClientStream;
use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::Duration;

static NEXT: AtomicUsize = AtomicUsize::new(0);

struct SocketDir(PathBuf);

impl SocketDir {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "deskwaifu-ipc-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&path).unwrap();
        Self(path)
    }

    fn socket(&self) -> PathBuf {
        self.0.join("test.sock")
    }
}

impl Drop for SocketDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn poll(server: &mut IpcServer, handler: &mut impl FnMut(&[u8]) -> Vec<u8>) -> usize {
    server
        .poll_once(Some(Duration::from_millis(5)), handler)
        .unwrap()
}

#[test]
fn dispatches_fragmented_and_buffered_lines_in_order() {
    let dir = SocketDir::new();
    let mut server = IpcServer::bind(dir.socket()).unwrap();
    let mut client = ClientStream::connect(dir.socket()).unwrap();
    client.write_all(b"first").unwrap();
    let mut seen = Vec::new();
    let mut handler = |line: &[u8]| {
        seen.push(line.to_vec());
        [b"reply:".as_slice(), line].concat()
    };
    poll(&mut server, &mut handler);
    poll(&mut server, &mut handler);
    client.write_all(b"\nsecond\n").unwrap();

    for _ in 0..4 {
        poll(&mut server, &mut handler);
    }
    let mut replies = [0; 25];
    client
        .set_read_timeout(Some(Duration::from_millis(100)))
        .unwrap();
    let count = client.read(&mut replies).unwrap();
    assert_eq!(&replies[..count], b"reply:first\nreply:second\n");
    assert_eq!(seen, [b"first".to_vec(), b"second".to_vec()]);
}

#[test]
fn accepts_exact_request_limit_and_closes_over_limit_connection() {
    let dir = SocketDir::new();
    let mut server = IpcServer::bind(dir.socket()).unwrap();
    let mut exact = ClientStream::connect(dir.socket()).unwrap();
    exact
        .write_all(&[vec![b'a'; MAX_REQUEST], b"\n".to_vec()].concat())
        .unwrap();
    let mut seen = Vec::new();
    let mut handler = |line: &[u8]| {
        seen.push(line.len());
        b"{}".to_vec()
    };
    for _ in 0..32 {
        poll(&mut server, &mut handler);
    }
    drop(handler);
    assert_eq!(seen, [MAX_REQUEST]);
    let mut response = [0; 3];
    exact.read_exact(&mut response).unwrap();
    assert_eq!(&response, b"{}\n");

    let mut over = ClientStream::connect(dir.socket()).unwrap();
    over.write_all(&[vec![b'b'; MAX_REQUEST + 1], b"\n".to_vec()].concat())
        .unwrap();
    let mut over_handler = |line: &[u8]| {
        seen.push(line.len());
        b"{}".to_vec()
    };
    for _ in 0..32 {
        poll(&mut server, &mut over_handler);
    }
    drop(over_handler);
    assert_eq!(seen, [MAX_REQUEST]);
    let mut byte = [0; 1];
    assert_eq!(over.read(&mut byte).unwrap(), 0);
}

#[test]
fn rejects_over_limit_response_for_only_that_connection() {
    let dir = SocketDir::new();
    let mut server = IpcServer::bind(dir.socket()).unwrap();
    let mut too_large = ClientStream::connect(dir.socket()).unwrap();
    too_large.write_all(b"large\n").unwrap();
    let mut handler = |line: &[u8]| {
        if line == b"large" {
            vec![b'x'; MAX_RESPONSE + 1]
        } else {
            b"ok".to_vec()
        }
    };
    for _ in 0..4 {
        poll(&mut server, &mut handler);
    }
    let mut byte = [0; 1];
    assert_eq!(too_large.read(&mut byte).unwrap(), 0);

    let mut healthy = ClientStream::connect(dir.socket()).unwrap();
    healthy.write_all(b"healthy\n").unwrap();
    for _ in 0..4 {
        poll(&mut server, &mut handler);
    }
    let mut response = [0; 3];
    healthy.read_exact(&mut response).unwrap();
    assert_eq!(&response, b"ok\n");
}

#[test]
fn slow_reader_does_not_block_a_fast_connection() {
    let dir = SocketDir::new();
    let mut server = IpcServer::bind(dir.socket()).unwrap();
    let mut slow = ClientStream::connect(dir.socket()).unwrap();
    slow.write_all(b"slow\nqueued\n").unwrap();
    let mut seen = Vec::new();
    let mut handler = |line: &[u8]| {
        seen.push(line.to_vec());
        if line == b"slow" {
            vec![b's'; MAX_RESPONSE]
        } else {
            b"fast".to_vec()
        }
    };
    poll(&mut server, &mut handler); // Accept slow connection.
    poll(&mut server, &mut handler); // Dispatch and partially write slow response.

    let mut fast = ClientStream::connect(dir.socket()).unwrap();
    fast.write_all(b"fast\n").unwrap();
    for _ in 0..5 {
        poll(&mut server, &mut handler);
    }
    drop(handler);
    assert_eq!(seen, [b"slow".to_vec(), b"fast".to_vec()]);
    let mut response = [0; 5];
    fast.set_read_timeout(Some(Duration::from_millis(100)))
        .unwrap();
    fast.read_exact(&mut response).unwrap();
    assert_eq!(&response, b"fast\n");
}

#[test]
fn drains_reply_after_client_half_closes() {
    let dir = SocketDir::new();
    let mut server = IpcServer::bind(dir.socket()).unwrap();
    let mut client = ClientStream::connect(dir.socket()).unwrap();
    client.write_all(b"request\n").unwrap();
    client.shutdown(std::net::Shutdown::Write).unwrap();
    let mut handler = |_: &[u8]| b"response".to_vec();
    for _ in 0..4 {
        poll(&mut server, &mut handler);
    }
    let mut response = Vec::new();
    client.read_to_end(&mut response).unwrap();
    assert_eq!(response, b"response\n");
}

#[test]
fn each_poll_dispatches_at_most_sixteen_requests() {
    let dir = SocketDir::new();
    let mut server = IpcServer::bind(dir.socket()).unwrap();
    let mut clients = Vec::new();
    for _ in 0..20 {
        let mut client = ClientStream::connect(dir.socket()).unwrap();
        client.write_all(b"request\n").unwrap();
        clients.push(client);
    }
    let mut handler = |_: &[u8]| b"ok".to_vec();
    assert_eq!(poll(&mut server, &mut handler), 4);
    assert!(server.poll_once(None, &mut handler).unwrap() <= 16);
    for _ in 0..16 {
        assert!(
            server
                .poll_once(Some(Duration::ZERO), &mut handler)
                .unwrap()
                <= 16
        );
    }
    for client in &mut clients {
        client
            .set_read_timeout(Some(Duration::from_millis(100)))
            .unwrap();
        let mut response = [0; 3];
        client.read_exact(&mut response).unwrap();
        assert_eq!(&response, b"ok\n");
    }
}
