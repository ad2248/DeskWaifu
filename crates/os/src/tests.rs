use super::*;

#[test]
fn connection_limit_applies_backpressure_and_recovers_after_expiry() {
    fs::create_dir_all("/tmp/opencode").unwrap();
    let directory = PathBuf::from(format!(
        "/tmp/opencode/deskwaifu-limit-{}",
        std::process::id()
    ));
    fs::create_dir(&directory).unwrap();
    let path = directory.join("ipc.sock");
    let mut server = Server::bind(&path).unwrap();
    let peers: Vec<_> = (0..MAX_CLIENTS + 1)
        .map(|_| UnixStream::connect(&path).unwrap())
        .collect();
    server.poll(|_| panic!("no requests sent")).unwrap();
    assert_eq!(server.clients.len(), MAX_CLIENTS);
    for client in &mut server.clients {
        client.active = Instant::now() - CLIENT_TIMEOUT;
    }
    server.poll(|_| panic!("no requests sent")).unwrap();
    assert!(server.clients.is_empty());
    server.poll(|_| panic!("no requests sent")).unwrap();
    assert_eq!(server.clients.len(), 1);
    drop(peers);
    drop(server);
    fs::remove_dir(directory).unwrap();
}
