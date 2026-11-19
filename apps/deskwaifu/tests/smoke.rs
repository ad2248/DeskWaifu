#![cfg(unix)]

#[path = "../../../tests/support/mod.rs"]
mod support;

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
    fn start(executable: &std::path::Path) -> Self {
        let child = Command::new(executable)
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

    fn stop(&mut self) {
        self.stop_with_signal("-INT");
    }

    fn stop_with_signal(&mut self, signal: &str) {
        assert!(
            Command::new("kill")
                .args([signal, &self.child.id().to_string()])
                .status()
                .unwrap()
                .success()
        );
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            if let Some(status) = self.child.try_wait().unwrap() {
                assert!(status.success());
                break;
            }
            assert!(Instant::now() < deadline, "platform did not stop");
            thread::sleep(Duration::from_millis(5));
        }
        assert!(fs::symlink_metadata(SOCKET_PATH).is_err());
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

fn call_cli(executable: &std::path::Path, args: &[&str]) -> Output {
    Command::new(executable)
        .env("XDG_RUNTIME_DIR", "/nonexistent/ignored-runtime")
        .args(args)
        .output()
        .unwrap()
}

// One test owns the fixed address: do not run several hosts in parallel, or
// remove an existing user socket merely to make the test pass.
#[test]
fn separate_cli_calls_dynamic_plugin_on_fixed_socket() {
    assert_eq!(SOCKET_PATH, "/tmp/deskwaifu.socket");
    assert!(
        fs::symlink_metadata(SOCKET_PATH).is_err(),
        "{SOCKET_PATH} already exists; refusing to disturb an existing platform"
    );
    // Deploy a copied executable and an independently built .so. Run from a
    // different working directory to verify lookup is relative to current_exe.
    let (build, _) = support::PluginBuild::new();
    let output = Command::new(env!("CARGO"))
        .current_dir(concat!(env!("CARGO_MANIFEST_DIR"), "/../.."))
        .args([
            "build",
            "--offline",
            "--locked",
            "-p",
            "deskwaifucli",
            "--target-dir",
        ])
        .arg(&build.target)
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    let cli = build.target.join("debug/deskwaifucli");
    let call = |args: &[&str]| call_cli(&cli, args);
    let output = call(&["test"]);
    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains("start 'deskwaifu' first"));
    assert!(output.stdout.is_empty());
    assert!(call(&[]).status.success());
    let output = call(&["--help"]);
    assert!(output.status.success());
    assert!(String::from_utf8_lossy(&output.stdout).contains("deskwaifucli test [ARGS...]"));
    let deployment = build.target.join("deployment");
    let plugins = deployment.join("plugins");
    fs::create_dir_all(&plugins).unwrap();
    let executable = deployment.join("deskwaifu");
    fs::copy(env!("CARGO_BIN_EXE_deskwaifu"), &executable).unwrap();
    fs::copy(build.plugin(), plugins.join("test.so")).unwrap();
    let launcher = build.target.join("launcher");
    std::os::unix::fs::symlink(&executable, &launcher).unwrap();
    let mut host = Host::start(&launcher);
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
    let output = Command::new(&executable).output().unwrap();
    assert!(!output.status.success());
    assert!(
        String::from_utf8_lossy(&output.stderr).contains("cannot listen on /tmp/deskwaifu.socket")
    );
    assert!(call(&["test"]).status.success());

    // SIGTERM (ordinary kill/service-manager shutdown) must run the same cleanup
    // as SIGINT, remove the socket, and allow an immediate restart with plugins.
    host.stop_with_signal("-TERM");
    drop(host);
    let mut restarted = Host::start(&executable);
    assert!(call(&["test"]).status.success());
    restarted.stop();
    drop(restarted);

    // Removing the .so removes the command: there is no statically linked fallback.
    fs::remove_file(plugins.join("test.so")).unwrap();
    let mut host = Host::start(&executable);
    let output = call(&["test"]);
    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains("unknown command: test"));
    assert!(call(&["help"]).status.success());
    host.stop();
    drop(host);

    // Bad libraries fail startup and release the socket rather than leaving a
    // partially loaded command set running.
    fs::write(plugins.join("invalid.so"), b"not a shared library").unwrap();
    let output = Command::new(&executable).output().unwrap();
    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains("load plugin"));
    assert!(fs::symlink_metadata(SOCKET_PATH).is_err());
}

#[test]
fn main_program_rejects_all_arguments_instead_of_dispatching_commands() {
    for args in [
        vec!["test"],
        vec!["help"],
        vec!["--help"],
        vec!["--socket", "/tmp/other.socket"],
        vec![""],
    ] {
        let output = Command::new(env!("CARGO_BIN_EXE_deskwaifu"))
            .args(&args)
            .output()
            .unwrap();
        assert!(!output.status.success(), "{args:?}");
        assert!(
            String::from_utf8_lossy(&output.stderr)
                .contains("does not accept arguments; use deskwaifucli")
        );
        assert!(output.stdout.is_empty());
    }
}
