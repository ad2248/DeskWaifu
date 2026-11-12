use serde_json::{json, Value};
use std::fs;
use std::io::{BufRead, BufReader, Read, Write};
use std::os::unix::{fs::symlink, net::UnixStream};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, ExitStatus, Stdio};
use std::time::{Duration, Instant};
use tempfile::{tempdir, TempDir};

const TIMEOUT: Duration = Duration::from_secs(10);

struct ChildGuard(Child);

impl ChildGuard {
    fn spawn(args: &[&std::ffi::OsStr]) -> Self {
        Self(
            Command::new(env!("CARGO_BIN_EXE_deskwaifu-rust"))
                .args(args)
                .stderr(Stdio::piped())
                .spawn()
                .unwrap(),
        )
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

    fn stderr(&mut self) -> String {
        let mut text = String::new();
        self.0
            .stderr
            .take()
            .unwrap()
            .read_to_string(&mut text)
            .unwrap();
        text
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
    _dir: TempDir,
}

impl Runtime {
    fn start(option: &str, model: &Path) -> Self {
        let dir = tempdir().unwrap();
        let socket = dir.path().join("runtime.sock");
        let child = ChildGuard::spawn(&[
            option.as_ref(),
            model.as_os_str(),
            "--socket".as_ref(),
            socket.as_os_str(),
        ]);
        let mut runtime = Self {
            child,
            socket,
            _dir: dir,
        };
        let deadline = Instant::now() + TIMEOUT;
        loop {
            if UnixStream::connect(&runtime.socket).is_ok() {
                return runtime;
            }
            if let Some(status) = runtime.child.0.try_wait().unwrap() {
                panic!("runtime exited ({status}): {}", runtime.child.stderr());
            }
            assert!(Instant::now() < deadline, "runtime did not bind in time");
            std::thread::sleep(Duration::from_millis(10));
        }
    }

    fn request(&self, request: Value) -> Value {
        let mut stream = UnixStream::connect(&self.socket).unwrap();
        stream.set_read_timeout(Some(TIMEOUT)).unwrap();
        stream.set_write_timeout(Some(TIMEOUT)).unwrap();
        writeln!(stream, "{request}").unwrap();
        let mut response = String::new();
        BufReader::new(stream).read_line(&mut response).unwrap();
        serde_json::from_str(&response).unwrap()
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

fn add_model(root: &Path, name: &str, fixture: &Path) -> PathBuf {
    let dir = root.join(name);
    fs::create_dir_all(&dir).unwrap();
    let mut descriptor: Value = serde_json::from_slice(&fs::read(fixture).unwrap()).unwrap();
    let moc = fixture
        .parent()
        .unwrap()
        .join(descriptor["FileReferences"]["Moc"].as_str().unwrap());
    let moc_name = format!("{name}.moc3");
    fs::copy(moc, dir.join(&moc_name)).unwrap();
    descriptor["FileReferences"]["Moc"] = json!(moc_name);
    // The catalog's directory name, not JSON Name or the filename, is the public model name.
    descriptor["Name"] = json!("not-the-catalog-name");
    let path = dir.join("different.model3.json");
    fs::write(&path, serde_json::to_vec(&descriptor).unwrap()).unwrap();
    path
}

#[test]
fn rejects_invalid_arguments_and_model_file() {
    let dir = tempdir().unwrap();
    let socket = dir.path().join("runtime.sock");
    let mut child = ChildGuard::spawn(&["--socket".as_ref(), socket.as_os_str()]);
    assert!(!child.wait().success());
    assert!(child.stderr().contains("exactly one"));

    let model = dir.path().join("Bad.model3.json");
    fs::write(&model, "{}").unwrap();
    let mut child = ChildGuard::spawn(&[
        "--model".as_ref(),
        model.as_os_str(),
        "--socket".as_ref(),
        socket.as_os_str(),
    ]);
    assert!(!child.wait().success());
    assert!(child.stderr().contains("invalid model3.json"));
}

#[test]
fn optional_haru_parameter_roundtrip_rejects_out_of_range_and_unknown_commands() {
    let Some(model) = fixture() else { return };
    let runtime = Runtime::start("--model", &model);
    let status = runtime.request(json!({"command":"get_status"}));
    assert_eq!(status["renderer_available"], false);
    assert_eq!(status["model_core_ready"], true);
    let parameters = runtime.request(json!({"command":"get_parameters"}));
    assert_eq!(parameters["ok"], true);
    let parameter = parameters["parameters"]
        .as_array()
        .unwrap()
        .iter()
        .find(|p| p["minimum"].as_f64().unwrap() < p["maximum"].as_f64().unwrap())
        .unwrap();
    let index = parameter["index"].as_u64().unwrap();
    let min = parameter["minimum"].as_f64().unwrap();
    let max = parameter["maximum"].as_f64().unwrap();
    let value = if parameter["value"].as_f64().unwrap() == min {
        max
    } else {
        min
    };
    let response =
        runtime.request(json!({"command":"set_parameter","args":{"index":index,"value":value}}));
    assert_eq!(response["ok"], true);
    assert_eq!(response["value"], value);
    let read = runtime.request(json!({"command":"get_parameters"}));
    assert_eq!(read["parameters"][index as usize]["value"], value);
    for invalid in [min - 1.0, max + 1.0] {
        let response = runtime
            .request(json!({"command":"set_parameter","args":{"index":index,"value":invalid}}));
        assert_eq!(response["ok"], false);
        assert!(response["error"]
            .as_str()
            .unwrap()
            .contains("minimum/maximum"));
    }
    let response = runtime.request(json!({"command":"set_parameter","args":{"index":parameters["parameters"].as_array().unwrap().len(),"value":0}}));
    assert_eq!(response["ok"], false);
    assert_eq!(response["error"], "parameter index out of bounds");
    let read = runtime.request(json!({"command":"get_parameters"}));
    assert_eq!(read["parameters"][index as usize]["value"], value);
    let response = runtime.request(json!({"command":"not_a_command"}));
    assert_eq!(response["ok"], false);
    assert_eq!(response["error"], "unsupported command: not_a_command");
    runtime.quit();
}

#[test]
fn optional_haru_set_model_loads_selection_and_preserves_previous_on_failure() {
    let Some(model) = fixture() else { return };
    let dir = tempdir().unwrap();
    add_model(dir.path(), "Alpha", &model);
    add_model(dir.path(), "Beta", &model);
    let broken = add_model(dir.path(), "Broken", &model);
    let mut descriptor: Value = serde_json::from_slice(&fs::read(&broken).unwrap()).unwrap();
    fs::write(broken.parent().unwrap().join("bad.moc3"), b"invalid moc").unwrap();
    descriptor["FileReferences"]["Moc"] = json!("bad.moc3");
    fs::write(broken, serde_json::to_vec(&descriptor).unwrap()).unwrap();
    let runtime = Runtime::start("--models-dir", dir.path());
    assert_eq!(
        runtime.request(json!({"command":"get_available_models"}))["models"],
        json!(["Alpha", "Beta", "Broken"])
    );
    assert_eq!(
        runtime.request(json!({"command":"get_current_model"}))["model"],
        "Alpha"
    );
    // Each descriptor references a distinct relative Moc filename.
    for (args, expected) in [
        (json!({"name":"Beta"}), "Beta"),
        (json!({"index":0}), "Alpha"),
        (json!({"index":1}), "Beta"),
    ] {
        let response = runtime.request(json!({"command":"set_model","args":args}));
        assert_eq!(response["ok"], true);
        assert_eq!(response["model"], expected);
        assert_eq!(
            runtime.request(json!({"command":"get_current_model"}))["model"],
            expected
        );
    }
    for args in [
        json!({"name":"missing"}),
        json!({"index":99}),
        json!({"index":-1}),
        json!({"name":"Broken"}),
    ] {
        assert_eq!(
            runtime.request(json!({"command":"set_model","args":args}))["ok"],
            false
        );
        assert_eq!(
            runtime.request(json!({"command":"get_current_model"}))["model"],
            "Beta"
        );
        assert_eq!(
            runtime.request(json!({"command":"get_parameters"}))["ok"],
            true
        );
    }
    runtime.quit();
}

#[test]
fn optional_haru_does_not_delete_preexisting_socket_path_file() {
    let Some(model) = fixture() else { return };
    let dir = tempdir().unwrap();
    let socket = dir.path().join("runtime.sock");
    fs::write(&socket, b"keep this file").unwrap();
    let mut child = ChildGuard::spawn(&[
        "--model".as_ref(),
        model.as_os_str(),
        "--socket".as_ref(),
        socket.as_os_str(),
    ]);
    assert!(!child.wait().success());
    assert!(child.stderr().contains("socket path already exists"));
    assert_eq!(fs::read(socket).unwrap(), b"keep this file");
}

#[test]
fn optional_haru_catalog_ignores_nested_models_and_symlink_loops() {
    let Some(model) = fixture() else { return };
    let dir = tempdir().unwrap();
    add_model(dir.path(), "Haru", &model);
    add_model(&dir.path().join("nested/deeper"), "Hidden", &model);
    symlink(dir.path(), dir.path().join("loop")).unwrap();
    symlink(dir.path().join("Haru"), dir.path().join("Haru/self")).unwrap();
    let runtime = Runtime::start("--models-dir", dir.path());
    assert_eq!(
        runtime.request(json!({"command":"get_available_models"}))["models"],
        json!(["Haru"])
    );
    runtime.quit();
}
