use super::*;
use std::sync::atomic::{AtomicUsize, Ordering};

fn server() -> Server {
    static NEXT: AtomicUsize = AtomicUsize::new(0);
    fs::create_dir_all("/tmp/opencode").unwrap();
    Server::bind(format!(
        "/tmp/opencode/deskwaifu-async-{}-{}.sock",
        std::process::id(),
        NEXT.fetch_add(1, Ordering::Relaxed)
    ))
    .unwrap()
}

async fn step(server: &mut Server, calls: &mut usize) {
    tokio::time::timeout(
        Duration::from_secs(2),
        server.poll_ready(|args| {
            *calls += 1;
            Ok(args.join(" "))
        }),
    )
    .await
    .expect("server must wake on I/O")
    .unwrap();
}

async fn response(server: &mut Server, peer: &mut UnixStream, calls: &mut usize) -> Response {
    let mut bytes = Vec::new();
    tokio::time::timeout(Duration::from_secs(2), async {
        loop {
            let mut chunk = [0; 4096];
            match peer.read(&mut chunk) {
                Ok(0) => panic!("unexpected EOF"),
                Ok(n) => {
                    bytes.extend_from_slice(&chunk[..n]);
                    if bytes.ends_with(b"\n") {
                        return serde_json::from_slice(&bytes).unwrap();
                    }
                }
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => step(server, calls).await,
                Err(e) => panic!("{e}"),
            }
        }
    })
    .await
    .unwrap()
}

#[tokio::test]
async fn failed_client_registration_leaves_existing_connection_usable() {
    let mut server = server();
    let mut existing = UnixStream::connect(&server.path).unwrap();
    existing.set_nonblocking(true).unwrap();
    existing
        .write_all(b"{\"source\":\"cli\",\"payload\":[\"first\"]}\n")
        .unwrap();
    let mut calls = 0;
    assert_eq!(
        response(&mut server, &mut existing, &mut calls).await,
        Response::Ok {
            output: "first".into()
        }
    );
    let rejected = UnixStream::connect(&server.path).unwrap();
    rejected.set_nonblocking(true).unwrap();
    tokio::time::timeout(Duration::from_secs(2), async {
        while server.clients.len() < 2 {
            step(&mut server, &mut calls).await;
        }
    })
    .await
    .unwrap();

    let rejected_id = server.clients[1].id;
    let mut disconnected = Vec::new();
    existing
        .write_all(b"{\"source\":\"cli\",\"payload\":[\"still alive\"]}\n")
        .unwrap();
    tokio::time::timeout(
        Duration::from_secs(2),
        server.poll_ready_with(
            |_, args| {
                calls += 1;
                Ok(args.join(" "))
            },
            |id| disconnected.push(id),
            |_| Err(io::Error::from_raw_os_error(libc::EMFILE)),
            |listener| listener.accept().map(|(stream, _)| stream),
        ),
    )
    .await
    .unwrap()
    .unwrap();
    assert_eq!(server.clients.len(), 1);
    assert_eq!(disconnected, [rejected_id]);
    // 其他测试并行启动子进程时，fork 到 exec 之间可能短暂持有套接字。
    let mut rejected = tokio::net::UnixStream::from_std(rejected).unwrap();
    let eof = tokio::time::timeout(
        Duration::from_secs(2),
        tokio::io::AsyncReadExt::read(&mut rejected, &mut [0]),
    )
    .await
    .expect("rejected connection must close")
    .unwrap();
    assert_eq!(eof, 0);
    assert_eq!(
        response(&mut server, &mut existing, &mut calls).await,
        Response::Ok {
            output: "still alive".into()
        }
    );
    assert_eq!(calls, 2);
}

#[tokio::test(start_paused = true)]
#[allow(deprecated)]
async fn descriptor_exhaustion_delays_accept_until_retry_deadline() {
    let mut server = server();
    let _peer = UnixStream::connect(&server.path).unwrap();
    server
        .poll_ready_with(
            |_, _| panic!("no request to dispatch"),
            |_| panic!("no connection to close"),
            |stream| AsyncFd::new(stream.try_clone()?),
            |_| Err(io::Error::from_raw_os_error(libc::EMFILE)),
        )
        .await
        .unwrap();
    let retry_at = server.accept_retry_at.unwrap();
    assert!(server.clients.is_empty());
    assert!(
        tokio::time::timeout(
            ACCEPT_RETRY_DELAY / 2,
            server.poll_ready(|_| unreachable!())
        )
        .await
        .is_err()
    );
    let mut calls = 0;
    step(&mut server, &mut calls).await;
    assert!(Instant::now() >= retry_at);
    assert_eq!(server.clients.len(), 1);
}

#[tokio::test]
async fn idle_wait_cancellation_partial_frames_and_persistent_connection() {
    let mut server = server();
    let mut calls = 0;
    assert!(
        tokio::time::timeout(
            Duration::from_millis(20),
            server.poll_ready(|_| { panic!("idle server must not dispatch") })
        )
        .await
        .is_err()
    );
    let mut peer = UnixStream::connect(&server.path).unwrap();
    peer.set_nonblocking(true).unwrap();
    peer.write_all(b"{\"source\":\"cli\",\"payload\":[\"hello")
        .unwrap();
    step(&mut server, &mut calls).await;
    // Consume initial reactor notification; then cancel a genuinely idle wait.
    assert!(
        tokio::time::timeout(Duration::from_millis(20), async {
            loop {
                step(&mut server, &mut calls).await;
            }
        })
        .await
        .is_err()
    );
    assert_eq!(calls, 0);
    assert!(!server.clients[0].input.is_empty());
    peer.write_all(b"\"]}\n").unwrap();
    assert_eq!(
        response(&mut server, &mut peer, &mut calls).await,
        Response::Ok {
            output: "hello".into()
        }
    );
    peer.write_all(b"{\"source\":\"cli\",\"payload\":[\"again\"]}\n")
        .unwrap();
    assert_eq!(
        response(&mut server, &mut peer, &mut calls).await,
        Response::Ok {
            output: "again".into()
        }
    );
    assert_eq!(calls, 2);
}

#[tokio::test]
async fn timeout_wakes_without_io_and_connection_limit_recovers() {
    let mut server = server();
    let peers: Vec<_> = (0..MAX_CLIENTS + 1)
        .map(|_| UnixStream::connect(&server.path).unwrap())
        .collect();
    let mut calls = 0;
    step(&mut server, &mut calls).await;
    assert_eq!(server.clients.len(), MAX_CLIENTS);
    for client in &mut server.clients {
        client.active = Instant::now() - CLIENT_TIMEOUT + Duration::from_millis(30);
    }
    tokio::time::timeout(Duration::from_secs(2), async {
        while !server.clients.is_empty() {
            step(&mut server, &mut calls).await;
        }
    })
    .await
    .unwrap();
    step(&mut server, &mut calls).await;
    assert_eq!(server.clients.len(), 1);
    assert_eq!(calls, 0);
    drop(peers);
}

#[tokio::test(start_paused = true)]
async fn partial_connection_expires_on_tokio_deadline_without_periodic_wakes() {
    let mut server = server();
    let mut peer = UnixStream::connect(&server.path).unwrap();
    peer.write_all(b"{\"source\":").unwrap();
    let mut calls = 0;
    step(&mut server, &mut calls).await;
    let active = server.clients[0].active;
    let id = server.clients[0].id;
    let mut disconnected = Vec::new();
    while !server.clients.is_empty() {
        server
            .poll_ready_connections(
                |_, _| panic!("partial frame must not dispatch"),
                |id| disconnected.push(id),
            )
            .await
            .unwrap();
    }
    assert!(active.elapsed() >= CLIENT_TIMEOUT);
    assert_eq!(disconnected, [id]);
    assert_eq!(calls, 0);
}

#[tokio::test]
async fn pipelined_large_frames_and_responses_survive_step_boundaries() {
    let mut server = server();
    let mut peer = UnixStream::connect(&server.path).unwrap();
    let text = "x".repeat(60 * 1024);
    let mut frames = serde_json::to_vec(&Request::cli(vec![text.clone()])).unwrap();
    frames.extend_from_slice(b"\n{\"source\":\"cli\",\"payload\":[\"next\"]}\n");
    peer.write_all(&frames).unwrap();
    peer.set_nonblocking(true).unwrap();
    let mut calls = 0;
    assert_eq!(
        response(&mut server, &mut peer, &mut calls).await,
        Response::Ok { output: text }
    );
    assert_eq!(
        response(&mut server, &mut peer, &mut calls).await,
        Response::Ok {
            output: "next".into()
        }
    );
    assert_eq!(calls, 2);
}

// 确认客户端停止读响应时，写入失败会关闭连接并通知命令所属的连接标识。
#[tokio::test]
async fn failed_response_write_notifies_disconnect() {
    let mut server = server();
    let mut peer = UnixStream::connect(&server.path).unwrap();
    peer.shutdown(std::net::Shutdown::Read).unwrap();
    peer.write_all(b"{\"source\":\"cli\",\"payload\":[\"request\"]}\n")
        .unwrap();

    let mut dispatched = Vec::new();
    let mut disconnected = Vec::new();
    tokio::time::timeout(Duration::from_secs(2), async {
        while disconnected.is_empty() {
            server
                .poll_ready_connections(
                    |id, _| {
                        dispatched.push(id);
                        Ok("response".into())
                    },
                    |id| disconnected.push(id),
                )
                .await
                .unwrap();
        }
    })
    .await
    .unwrap();
    assert_eq!(dispatched.len(), 1);
    assert_eq!(disconnected, dispatched);
    assert!(server.clients.is_empty());
}

#[tokio::test]
async fn oversized_and_half_closed_frames_return_errors() {
    for oversized in [false, true] {
        let mut server = server();
        let mut peer = UnixStream::connect(&server.path).unwrap();
        if oversized {
            peer.write_all(&vec![b'x'; MAX_FRAME_BYTES + 1]).unwrap();
        } else {
            peer.write_all(b"{\"source\":").unwrap();
            peer.shutdown(std::net::Shutdown::Write).unwrap();
        }
        peer.set_nonblocking(true).unwrap();
        let mut calls = 0;
        let reply = response(&mut server, &mut peer, &mut calls).await;
        let Response::Error { error } = reply else {
            panic!("expected error")
        };
        assert!(error.contains(if oversized { "64 KiB" } else { "incomplete" }));
        assert_eq!(calls, 0);
    }
}
