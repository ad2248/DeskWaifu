#![cfg(unix)]

use deskwaifu_os::SOCKET_PATH;
use std::{
    fs,
    os::unix::fs::MetadataExt,
    process::{Child, Command, Output, Stdio},
    thread,
    time::{Duration, Instant},
};

struct Host {
    child: Child,
    identity: Option<(u64, u64)>,
}

impl Host {
    fn start() -> Self {
        let child = Command::new(env!("CARGO_BIN_EXE_deskwaifu"))
            .env_remove("XDG_RUNTIME_DIR")
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .unwrap();
        let mut host = Self {
            child,
            identity: None,
        };
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            assert!(
                host.child.try_wait().unwrap().is_none(),
                "platform exited before binding"
            );
            if let Ok(meta) = fs::symlink_metadata(SOCKET_PATH) {
                host.identity = Some((meta.dev(), meta.ino()));
                return host;
            }
            assert!(Instant::now() < deadline, "platform did not bind");
            thread::sleep(Duration::from_millis(5));
        }
    }
}

impl Drop for Host {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
        // Panic fallback: never remove another process's socket.
        if fs::symlink_metadata(SOCKET_PATH)
            .is_ok_and(|meta| Some((meta.dev(), meta.ino())) == self.identity)
        {
            let _ = fs::remove_file(SOCKET_PATH);
        }
    }
}

fn call(args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_deskwaifu"))
        .env("XDG_RUNTIME_DIR", "/nonexistent/ignored-runtime")
        .args(args)
        .output()
        .unwrap()
}

// One test owns the fixed address: do not run several hosts in parallel, or
// remove an existing user socket merely to make the test pass.
#[test]
fn deskwaifu_test_uses_fixed_socket_and_default_invocation_starts_platform() {
    assert_eq!(SOCKET_PATH, "/tmp/deskwaifu.socket");
    assert!(
        fs::symlink_metadata(SOCKET_PATH).is_err(),
        "{SOCKET_PATH} already exists; refusing to disturb an existing platform"
    );
    let output = call(&["test"]);
    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains("start 'deskwaifu' first"));
    assert!(output.stdout.is_empty());

    let mut host = Host::start();
    let output = call(&["test"]);
    assert!(output.status.success(), "{output:?}");
    assert_eq!(
        String::from_utf8(output.stdout).unwrap(),
        "test command received 0 argument(s): []\n"
    );

    let output = call(&["help"]);
    assert!(output.status.success(), "{output:?}");
    let stdout = String::from_utf8(output.stdout).unwrap();
    assert!(stdout.contains("help  Show available commands"), "{stdout}");
    assert!(stdout.contains("test  Run the test command."), "{stdout}");

    let output = call(&["test", "你好 world", "", "line\nbreak", "--socket"]);
    assert!(output.status.success(), "{output:?}");
    assert_eq!(
        String::from_utf8(output.stdout).unwrap(),
        "test command received 4 argument(s): [\"你好 world\", \"\", \"line\\nbreak\", \"--socket\"]\n"
    );
    for command in ["missing", "serve", "--socket"] {
        let output = call(&[command]);
        assert!(!output.status.success());
        assert!(
            String::from_utf8_lossy(&output.stderr)
                .contains(&format!("unknown command: {command}"))
        );
    }
    // A second no-argument invocation must not replace the existing listener.
    let output = call(&[]);
    assert!(!output.status.success());
    assert!(
        String::from_utf8_lossy(&output.stderr).contains("cannot listen on /tmp/deskwaifu.socket")
    );
    assert!(call(&["test"]).status.success());

    assert!(
        Command::new("kill")
            .args(["-INT", &host.child.id().to_string()])
            .status()
            .unwrap()
            .success()
    );
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        if let Some(status) = host.child.try_wait().unwrap() {
            assert!(status.success());
            break;
        }
        assert!(Instant::now() < deadline, "platform did not stop");
        thread::sleep(Duration::from_millis(5));
    }
    assert!(fs::symlink_metadata(SOCKET_PATH).is_err());
}

#[test]
fn launcher_help_describes_direct_commands_without_configuration() {
    let output = call(&["--help"]);
    assert!(output.status.success());
    let stdout = String::from_utf8(output.stdout).unwrap();
    assert!(stdout.contains("deskwaifu test [ARGS...]"));
    assert!(stdout.contains("/tmp/deskwaifu.socket"));
    assert!(!stdout.contains("--socket"));
    assert!(!stdout.contains("serve"));
}
