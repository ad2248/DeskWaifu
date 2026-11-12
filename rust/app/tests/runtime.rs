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
        self.request_text(&request.to_string())
    }

    /// Sends request text verbatim, for values `serde_json` cannot build (such
    /// as a literal that overflows `f64`).
    fn request_text(&self, request: &str) -> Value {
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

/// Adds a catalog entry whose referenced Moc cannot be loaded, so selecting it fails.
fn add_broken_model(root: &Path, name: &str, fixture: &Path) -> PathBuf {
    let path = add_model(root, name, fixture);
    let mut descriptor: Value = serde_json::from_slice(&fs::read(&path).unwrap()).unwrap();
    fs::write(path.parent().unwrap().join("bad.moc3"), b"invalid moc").unwrap();
    descriptor["FileReferences"]["Moc"] = json!("bad.moc3");
    fs::write(&path, serde_json::to_vec(&descriptor).unwrap()).unwrap();
    path
}

/// Writes a `.vtube.json` next to the model's `.model3.json`; `add_model` names
/// that file `different.model3.json`, so the configuration is `different.vtube.json`.
fn write_vtube_config(model_dir: &Path, hotkeys: Value) {
    fs::write(
        model_dir.join("different.vtube.json"),
        serde_json::to_vec(&json!({ "Hotkeys": hotkeys })).unwrap(),
    )
    .unwrap();
}

/// Writes an expression file the hotkey resolver can load, under `Expressions/`
/// so the binding exercises the `Expressions/` fallback directory.
fn write_expression(model_dir: &Path, name: &str, parameter: &str) {
    let directory = model_dir.join("Expressions");
    fs::create_dir_all(&directory).unwrap();
    fs::write(
        directory.join(name),
        serde_json::to_vec(&json!({
            "Type": "Live2D Expression",
            "Parameters": [{"Id": parameter, "Value": 1, "Blend": "Add"}],
        }))
        .unwrap(),
    )
    .unwrap();
}

/// The `active` flag of the hotkey with `id` in a `get_hotkeys`/`trigger_hotkey` reply.
fn hotkey_active(reply: &Value, id: &str) -> Option<bool> {
    reply["hotkeys"]
        .as_array()
        .unwrap()
        .iter()
        .find(|entry| entry["id"] == id)
        .and_then(|entry| entry["active"].as_bool())
}

/// A live connection, so streaming sources stay owned while the test writes.
struct Client {
    reader: BufReader<UnixStream>,
}

impl Client {
    fn connect(runtime: &Runtime) -> Self {
        let stream = UnixStream::connect(&runtime.socket).unwrap();
        stream.set_read_timeout(Some(TIMEOUT)).unwrap();
        stream.set_write_timeout(Some(TIMEOUT)).unwrap();
        Self {
            reader: BufReader::new(stream),
        }
    }

    fn request(&mut self, request: Value) -> Value {
        writeln!(self.reader.get_mut(), "{request}").unwrap();
        let mut response = String::new();
        self.reader.read_line(&mut response).unwrap();
        assert!(
            !response.is_empty(),
            "connection closed without replying to {request}"
        );
        serde_json::from_str(&response).unwrap()
    }
}

/// Polls control status until `settled` holds, e.g. after a disconnect is processed.
fn wait_for_control(runtime: &Runtime, mut settled: impl FnMut(&Value) -> bool) -> Value {
    let deadline = Instant::now() + TIMEOUT;
    loop {
        let status = runtime.request(json!({"command":"get_control_status"}));
        if settled(&status) {
            return status;
        }
        assert!(
            Instant::now() < deadline,
            "control status stuck at {status}"
        );
        std::thread::sleep(Duration::from_millis(10));
    }
}

fn no_control() -> Value {
    json!({"ok":true,"has_mouth":false,"mouth":0.0,"has_look":false,"look_x":0.0,"look_y":0.0})
}

/// Control values are `f32` in the arbiter, so compare them at that precision.
fn float(status: &Value, key: &str) -> f32 {
    status[key].as_f64().unwrap() as f32
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
    add_broken_model(dir.path(), "Broken", &model);
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

#[test]
fn optional_haru_control_roundtrip_clamps_and_resets_one_channel() {
    let Some(model) = fixture() else { return };
    let runtime = Runtime::start("--model", &model);
    assert_eq!(
        runtime.request(json!({"command":"get_control_status"})),
        no_control()
    );
    // Every request below arrives on a fresh connection, so these writes use the
    // shared persistent source and survive the writer's connection closing.
    assert_eq!(
        runtime.request(json!({"command":"set_mouth_y","args":{"value":0.4}}))["ok"],
        true
    );
    assert_eq!(
        runtime.request(json!({"command":"set_look","args":{"x":-0.5,"y":0.25}}))["ok"],
        true
    );
    let status = runtime.request(json!({"command":"get_control_status"}));
    assert_eq!(status["has_mouth"], true);
    assert_eq!(float(&status, "mouth"), 0.4);
    assert_eq!(status["has_look"], true);
    assert_eq!(float(&status, "look_x"), -0.5);
    assert_eq!(float(&status, "look_y"), 0.25);

    // Out-of-range mouth values are clamped instead of rejected.
    for (value, expected) in [(1.5, 1.0), (-0.25, 0.0)] {
        assert_eq!(
            runtime.request(json!({"command":"set_mouth_y","args":{"value":value}}))["ok"],
            true
        );
        let status = runtime.request(json!({"command":"get_control_status"}));
        assert_eq!(float(&status, "mouth"), expected);
    }

    // Missing look coordinates default to 0.0.
    assert_eq!(
        runtime.request(json!({"command":"set_look","args":{}}))["ok"],
        true
    );
    let status = runtime.request(json!({"command":"get_control_status"}));
    assert_eq!(float(&status, "look_x"), 0.0);
    assert_eq!(float(&status, "look_y"), 0.0);

    // A reset clears only the channel it names. The legacy C++ boolean spellings
    // (`1`, `"true"`, `"1"`) stay accepted.
    assert_eq!(
        runtime.request(json!({"command":"set_mouth_y","args":{"value":0.6}}))["ok"],
        true
    );
    assert_eq!(
        runtime.request(json!({"command":"set_look","args":{"x":0.1,"y":0.2}}))["ok"],
        true
    );
    assert_eq!(
        runtime.request(json!({"command":"set_mouth_y","args":{"reset":"1"}}))["ok"],
        true
    );
    let status = runtime.request(json!({"command":"get_control_status"}));
    assert_eq!(status["has_mouth"], false);
    assert_eq!(status["has_look"], true);
    assert_eq!(float(&status, "look_x"), 0.1);
    assert_eq!(
        runtime.request(json!({"command":"set_look","args":{"reset":1}}))["ok"],
        true
    );
    assert_eq!(
        runtime.request(json!({"command":"get_control_status"})),
        no_control()
    );

    // release_source clears both channels of the calling source.
    assert_eq!(
        runtime.request(json!({"command":"set_mouth_y","args":{"value":0.6}}))["ok"],
        true
    );
    assert_eq!(
        runtime.request(json!({"command":"set_look","args":{"x":0.1,"y":0.2}}))["ok"],
        true
    );
    assert_eq!(
        runtime.request(json!({"command":"release_source"}))["ok"],
        true
    );
    assert_eq!(
        runtime.request(json!({"command":"get_control_status"})),
        no_control()
    );
    runtime.quit();
}

#[test]
fn optional_haru_stream_outranks_persistent_until_it_disconnects() {
    let Some(model) = fixture() else { return };
    let runtime = Runtime::start("--model", &model);
    assert_eq!(
        runtime.request(json!({"command":"set_mouth_y","args":{"value":0.3}}))["ok"],
        true
    );
    let mut stream = Client::connect(&runtime);
    assert_eq!(
        stream.request(json!({"command":"set_mouth_y","args":{"value":0.8,"stream_source":true}}))
            ["ok"],
        true
    );
    let status = runtime.request(json!({"command":"get_control_status"}));
    assert_eq!(float(&status, "mouth"), 0.8);
    drop(stream);
    // Once the owning connection is gone the persistent value is effective again.
    let status = wait_for_control(&runtime, |status| float(status, "mouth") == 0.3);
    assert_eq!(status["has_mouth"], true);
    runtime.quit();
}

#[test]
fn optional_haru_streaming_connections_do_not_leak_into_each_other() {
    let Some(model) = fixture() else { return };
    let runtime = Runtime::start("--model", &model);
    assert_eq!(
        runtime.request(json!({"command":"set_mouth_y","args":{"value":0.3}}))["ok"],
        true
    );
    assert_eq!(
        runtime.request(json!({"command":"set_look","args":{"x":0.1,"y":0.2}}))["ok"],
        true
    );
    let mut a = Client::connect(&runtime);
    let mut b = Client::connect(&runtime);
    assert_eq!(
        a.request(json!({"command":"set_mouth_y","args":{"value":0.8,"stream_source":true}}))["ok"],
        true
    );
    assert_eq!(
        a.request(json!({"command":"set_look","args":{"x":0.5,"y":0.5,"stream_source":true}}))
            ["ok"],
        true
    );
    assert_eq!(
        b.request(json!({"command":"set_look","args":{"x":-0.4,"y":-0.5,"stream_source":true}}))
            ["ok"],
        true
    );
    // The latest stream write wins within the same class, regardless of owner.
    let status = runtime.request(json!({"command":"get_control_status"}));
    assert_eq!(float(&status, "mouth"), 0.8);
    assert_eq!(float(&status, "look_x"), -0.4);

    // B releasing its own source does not touch A's mouth, nor the persistent look.
    assert_eq!(
        b.request(json!({"command":"release_source","args":{"stream_source":true}}))["ok"],
        true
    );
    let status = runtime.request(json!({"command":"get_control_status"}));
    assert_eq!(float(&status, "mouth"), 0.8);
    assert_eq!(float(&status, "look_x"), 0.5);
    assert_eq!(
        b.request(json!({"command":"set_look","args":{"x":-0.1,"y":-0.2,"stream_source":true}}))
            ["ok"],
        true
    );

    // Disconnecting A releases only A's sources; B is unaffected.
    drop(a);
    let status = wait_for_control(&runtime, |status| float(status, "mouth") == 0.3);
    assert_eq!(status["has_mouth"], true);
    assert_eq!(float(&status, "look_x"), -0.1);
    assert_eq!(float(&status, "look_y"), -0.2);

    drop(b);
    let status = wait_for_control(&runtime, |status| float(status, "look_x") == 0.1);
    assert_eq!(float(&status, "mouth"), 0.3);
    runtime.quit();
}

#[test]
fn optional_haru_real_model_switch_clears_controls_and_reselection_keeps_them() {
    let Some(model) = fixture() else { return };
    let dir = tempdir().unwrap();
    add_model(dir.path(), "Alpha", &model);
    add_model(dir.path(), "Beta", &model);
    add_broken_model(dir.path(), "Broken", &model);
    let runtime = Runtime::start("--models-dir", dir.path());
    let set_controls = || {
        assert_eq!(
            runtime.request(json!({"command":"set_mouth_y","args":{"value":0.4}}))["ok"],
            true
        );
        assert_eq!(
            runtime.request(json!({"command":"set_look","args":{"x":0.1,"y":0.2}}))["ok"],
            true
        );
    };
    let has_controls = |status: &Value| {
        status["has_mouth"] == true
            && float(status, "mouth") == 0.4
            && status["has_look"] == true
            && float(status, "look_x") == 0.1
    };
    set_controls();
    // Selecting the current model keeps every external source.
    assert_eq!(
        runtime.request(json!({"command":"set_model","args":{"name":"Alpha"}}))["ok"],
        true
    );
    let status = runtime.request(json!({"command":"get_control_status"}));
    assert!(has_controls(&status), "{status}");
    // A failed load keeps them as well.
    assert_eq!(
        runtime.request(json!({"command":"set_model","args":{"name":"Broken"}}))["ok"],
        false
    );
    let status = runtime.request(json!({"command":"get_control_status"}));
    assert!(has_controls(&status), "{status}");
    // A successful switch to a different model clears them.
    assert_eq!(
        runtime.request(json!({"command":"set_model","args":{"name":"Beta"}}))["ok"],
        true
    );
    assert_eq!(
        runtime.request(json!({"command":"get_control_status"})),
        no_control()
    );
    set_controls();
    assert_eq!(
        runtime.request(json!({"command":"set_model","args":{"index":1}}))["ok"],
        true
    );
    let status = runtime.request(json!({"command":"get_control_status"}));
    assert!(has_controls(&status), "{status}");
    runtime.quit();
}

#[test]
fn optional_haru_invalid_control_requests_leave_state_unchanged() {
    let Some(model) = fixture() else { return };
    let runtime = Runtime::start("--model", &model);
    assert_eq!(
        runtime.request(json!({"command":"set_mouth_y","args":{"value":0.4}}))["ok"],
        true
    );
    assert_eq!(
        runtime.request(json!({"command":"set_look","args":{"x":0.1,"y":0.2}}))["ok"],
        true
    );
    let expected = runtime.request(json!({"command":"get_control_status"}));
    for request in [
        json!({"command":"set_mouth_y","args":{"value":"0.5"}}),
        json!({"command":"set_mouth_y","args":{"value":null}}),
        json!({"command":"set_mouth_y","args":{"value":0.5,"stream_source":"invalid"}}),
        json!({"command":"set_mouth_y","args":{"reset":"yes"}}),
        json!({"command":"set_look","args":{"x":"bad"}}),
        json!({"command":"set_look","args":{"y":true}}),
        json!({"command":"set_look","args":{"x":null,"y":0.0}}),
        json!({"command":"release_source","args":{"stream_source":"invalid"}}),
        json!({"command":"release_source","args":{"stream_source":2}}),
    ] {
        assert_eq!(runtime.request(request.clone())["ok"], false, "{request}");
        assert_eq!(
            runtime.request(json!({"command":"get_control_status"})),
            expected,
            "{request}"
        );
    }
    runtime.quit();
}

/// Writes one mouth and one look value through the shared persistent source and
/// asserts both arrived, so a later check can tell whether they survived.
fn set_controls(runtime: &Runtime) {
    assert_eq!(
        runtime.request(json!({"command":"set_mouth_y","args":{"value":0.4}}))["ok"],
        true
    );
    assert_eq!(
        runtime.request(json!({"command":"set_look","args":{"x":0.1,"y":0.2}}))["ok"],
        true
    );
    let status = runtime.request(json!({"command":"get_control_status"}));
    assert_eq!(float(&status, "mouth"), 0.4);
    assert_eq!(float(&status, "look_x"), 0.1);
}

#[test]
fn optional_haru_next_and_prev_wrap_in_catalog_name_order() {
    let Some(model) = fixture() else { return };
    let dir = tempdir().unwrap();
    // Written out of order; the catalog sorts by name, so Alpha..Gamma is the order.
    for name in ["Gamma", "Alpha", "Beta"] {
        add_model(dir.path(), name, &model);
    }
    let runtime = Runtime::start("--models-dir", dir.path());
    assert_eq!(
        runtime.request(json!({"command":"get_available_models"}))["models"],
        json!(["Alpha", "Beta", "Gamma"])
    );
    let current = || runtime.request(json!({"command":"get_current_model"}))["model"].clone();
    assert_eq!(current(), "Alpha");
    for (command, expected, index) in [
        ("next_model", "Beta", 1),
        ("next_model", "Gamma", 2),
        ("next_model", "Alpha", 0),
        ("prev_model", "Gamma", 2),
        ("prev_model", "Beta", 1),
        ("prev_model", "Alpha", 0),
    ] {
        let response = runtime.request(json!({"command":command}));
        assert_eq!(response, json!({"ok":true,"model":expected}), "{command}");
        assert_eq!(current(), expected);
        let status = runtime.request(json!({"command":"get_status"}));
        assert_eq!(status["model"], expected);
        assert_eq!(status["model_index"], index);
        assert_eq!(status["model_count"], 3);
    }
    runtime.quit();
}

#[test]
fn optional_haru_next_and_prev_reselect_the_only_model_and_keep_controls() {
    let Some(model) = fixture() else { return };
    let runtime = Runtime::start("--model", &model);
    let name = runtime.request(json!({"command":"get_current_model"}))["model"].clone();
    set_controls(&runtime);
    for command in ["next_model", "prev_model"] {
        let response = runtime.request(json!({"command":command}));
        assert_eq!(response, json!({"ok":true,"model":name}), "{command}");
        // Re-selecting the same identity must not clear external controls.
        let status = runtime.request(json!({"command":"get_control_status"}));
        assert_eq!(float(&status, "mouth"), 0.4);
        assert_eq!(float(&status, "look_x"), 0.1);
        assert_eq!(float(&status, "look_y"), 0.2);
    }
    runtime.quit();
}

#[test]
fn optional_haru_next_and_prev_fail_on_broken_neighbor_and_keep_state() {
    let Some(model) = fixture() else { return };
    let dir = tempdir().unwrap();
    add_model(dir.path(), "Alpha", &model);
    add_model(dir.path(), "Beta", &model);
    add_broken_model(dir.path(), "Broken", &model);
    let runtime = Runtime::start("--models-dir", dir.path());
    let current = || runtime.request(json!({"command":"get_current_model"}))["model"].clone();
    assert_eq!(
        runtime.request(json!({"command":"get_available_models"}))["models"],
        json!(["Alpha", "Beta", "Broken"])
    );
    set_controls(&runtime);
    // Alpha -> Beta succeeds.
    assert_eq!(runtime.request(json!({"command":"next_model"}))["ok"], true);
    assert_eq!(current(), "Beta");
    set_controls(&runtime);
    // Beta -> Broken fails and keeps both the model and its controls.
    assert_eq!(
        runtime.request(json!({"command":"next_model"}))["ok"],
        false
    );
    assert_eq!(current(), "Beta");
    let status = runtime.request(json!({"command":"get_control_status"}));
    assert_eq!(float(&status, "mouth"), 0.4);
    // The same holds when the broken model is the previous one.
    assert_eq!(
        runtime.request(json!({"command":"set_model","args":{"name":"Alpha"}}))["ok"],
        true
    );
    set_controls(&runtime);
    assert_eq!(
        runtime.request(json!({"command":"prev_model"}))["ok"],
        false
    );
    assert_eq!(current(), "Alpha");
    let status = runtime.request(json!({"command":"get_control_status"}));
    assert_eq!(float(&status, "mouth"), 0.4);
    assert_eq!(float(&status, "look_x"), 0.1);
    runtime.quit();
}

#[test]
fn optional_haru_next_model_change_clears_controls_and_accepts_new_writes() {
    let Some(model) = fixture() else { return };
    let dir = tempdir().unwrap();
    add_model(dir.path(), "Alpha", &model);
    add_model(dir.path(), "Beta", &model);
    let runtime = Runtime::start("--models-dir", dir.path());
    set_controls(&runtime);
    assert_eq!(runtime.request(json!({"command":"next_model"}))["ok"], true);
    assert_eq!(
        runtime.request(json!({"command":"get_control_status"})),
        no_control()
    );
    // The arbiter accepts new writes for the newly loaded identity.
    assert_eq!(
        runtime.request(json!({"command":"set_mouth_y","args":{"value":0.7}}))["ok"],
        true
    );
    let status = runtime.request(json!({"command":"get_control_status"}));
    assert_eq!(status["has_mouth"], true);
    assert_eq!(float(&status, "mouth"), 0.7);
    runtime.quit();
}

#[test]
fn optional_haru_status_and_model_position_report_defaults() {
    let Some(model) = fixture() else { return };
    let runtime = Runtime::start("--model", &model);
    let name = runtime.request(json!({"command":"get_current_model"}))["model"].clone();
    let status = runtime.request(json!({"command":"get_status"}));
    let mut keys: Vec<_> = status
        .as_object()
        .unwrap()
        .keys()
        .map(String::as_str)
        .collect();
    keys.sort_unstable();
    assert_eq!(
        keys,
        [
            "hidden",
            "model",
            "model_core_ready",
            "model_count",
            "model_index",
            "ok",
            "renderer_available",
            "x",
            "y",
            "zoom",
        ]
    );
    assert_eq!(status["ok"], true);
    assert_eq!(status["renderer_available"], false);
    assert_eq!(status["model_core_ready"], true);
    assert_eq!(status["model"], name);
    assert_eq!(status["model_index"], 0);
    assert_eq!(status["model_count"], 1);
    assert_eq!(status["hidden"], false);
    assert_eq!(float(&status, "zoom"), 1.0);
    assert_eq!(float(&status, "x"), 0.0);
    assert_eq!(float(&status, "y"), 0.0);
    let position = runtime.request(json!({"command":"get_model_position"}));
    let mut keys: Vec<_> = position
        .as_object()
        .unwrap()
        .keys()
        .map(String::as_str)
        .collect();
    keys.sort_unstable();
    assert_eq!(keys, ["ok", "x", "y", "zoom"]);
    assert_eq!(position["ok"], true);
    assert_eq!(float(&position, "zoom"), 1.0);
    assert_eq!(float(&position, "x"), 0.0);
    assert_eq!(float(&position, "y"), 0.0);
    let response = runtime.request(json!({"command":"not_a_command"}));
    assert_eq!(response["ok"], false);
    assert_eq!(response["error"], "unsupported command: not_a_command");
    runtime.quit();
}

#[test]
fn optional_haru_set_model_position_updates_partially_and_rejects_invalid() {
    let Some(model) = fixture() else { return };
    let runtime = Runtime::start("--model", &model);
    assert_eq!(
        runtime.request(json!({"command":"set_model_position","args":{"x":0.5}}))["ok"],
        true
    );
    let position = runtime.request(json!({"command":"get_model_position"}));
    assert_eq!(float(&position, "x"), 0.5);
    assert_eq!(float(&position, "y"), 0.0);
    assert_eq!(float(&position, "zoom"), 1.0);
    assert_eq!(
        runtime.request(json!({"command":"set_model_position","args":{"y":-0.25}}))["ok"],
        true
    );
    let position = runtime.request(json!({"command":"get_model_position"}));
    assert_eq!(float(&position, "x"), 0.5);
    assert_eq!(float(&position, "y"), -0.25);
    // An empty request keeps both stored values.
    assert_eq!(
        runtime.request(json!({"command":"set_model_position","args":{}}))["ok"],
        true
    );
    assert_eq!(
        runtime.request(json!({"command":"get_model_position"})),
        position
    );
    let expected = runtime.request(json!({"command":"get_status"}));
    for args in [
        json!({"x":"0.5"}),
        json!({"x":null}),
        json!({"y":true}),
        json!({"y":[1]}),
        // Finite as f64 but infinite once narrowed to the f32 boundary.
        json!({"x":1e39}),
    ] {
        let request = json!({"command":"set_model_position","args":args});
        assert_eq!(runtime.request(request.clone())["ok"], false, "{request}");
        assert_eq!(
            runtime.request(json!({"command":"get_status"})),
            expected,
            "{request}"
        );
    }
    // A literal that overflows f64 is rejected while parsing the request.
    let response = runtime.request_text(r#"{"command":"set_model_position","args":{"x":1e400}}"#);
    assert_eq!(response["ok"], false);
    assert_eq!(runtime.request(json!({"command":"get_status"})), expected);
    runtime.quit();
}

#[test]
fn optional_haru_set_model_zoom_accepts_boundaries_and_rejects_outside() {
    let Some(model) = fixture() else { return };
    let runtime = Runtime::start("--model", &model);
    for value in [10.0, 0.1] {
        let response = runtime.request(json!({"command":"set_model_zoom","args":{"value":value}}));
        assert_eq!(response, json!({"ok":true}), "{value}");
        assert_eq!(
            float(
                &runtime.request(json!({"command":"get_model_position"})),
                "zoom"
            ),
            value as f32
        );
        assert_eq!(
            float(&runtime.request(json!({"command":"get_status"})), "zoom"),
            value as f32
        );
    }
    let expected = runtime.request(json!({"command":"get_model_position"}));
    for (args, error) in [
        (json!({"value":0.09}), "value must be 0.1..10.0"),
        (json!({"value":10.01}), "value must be 0.1..10.0"),
        (json!({"value":0}), "value must be 0.1..10.0"),
        (json!({"value":-1}), "value must be 0.1..10.0"),
        // Missing value keeps the C++ -1.0 fallback, which is out of range.
        (json!({}), "value must be 0.1..10.0"),
        // `JsonGetFloat` reports its own message for values that are not finite numbers.
        (json!({"value":"1.5"}), "value must be a finite number"),
        (json!({"value":null}), "value must be a finite number"),
        (json!({"value":false}), "value must be a finite number"),
        // Infinite once narrowed to f32.
        (json!({"value":1e39}), "value must be a finite number"),
    ] {
        let request = json!({"command":"set_model_zoom","args":args});
        let response = runtime.request(request.clone());
        assert_eq!(response["ok"], false, "{request}");
        assert_eq!(response["error"], error, "{request}");
        assert_eq!(
            runtime.request(json!({"command":"get_model_position"})),
            expected,
            "{request}"
        );
    }
    runtime.quit();
}

#[test]
fn optional_haru_toggle_hidden_flips_twice_back_to_visible() {
    let Some(model) = fixture() else { return };
    let runtime = Runtime::start("--model", &model);
    assert_eq!(
        runtime.request(json!({"command":"toggle_hidden"})),
        json!({"ok":true,"hidden":true})
    );
    assert_eq!(
        runtime.request(json!({"command":"get_status"}))["hidden"],
        true
    );
    assert_eq!(
        runtime.request(json!({"command":"toggle_hidden"})),
        json!({"ok":true,"hidden":false})
    );
    assert_eq!(
        runtime.request(json!({"command":"get_status"}))["hidden"],
        false
    );
    runtime.quit();
}

#[test]
fn optional_haru_get_motions_lists_declared_groups_in_order() {
    let Some(model) = fixture() else { return };
    let runtime = Runtime::start("--model", &model);
    let response = runtime.request(json!({"command":"get_motions"}));
    assert_eq!(response["ok"], true);
    let motions = response["motions"].as_array().unwrap();
    // Haru declares Idle (2 motions) then TapBody (4 motions).
    assert_eq!(motions.len(), 6);
    assert_eq!(
        motions[0],
        json!({"group":"Idle","index":0,"file":"motions/haru_g_idle.motion3.json"})
    );
    assert_eq!(
        motions[1],
        json!({"group":"Idle","index":1,"file":"motions/haru_g_m15.motion3.json"})
    );
    assert_eq!(motions[2]["group"], "TapBody");
    assert_eq!(motions[2]["index"], 0);
    assert_eq!(motions[2]["file"], "motions/haru_g_m26.motion3.json");
    // Indices restart per group and groups stay in declaration order.
    assert_eq!(motions[5]["group"], "TapBody");
    assert_eq!(motions[5]["index"], 3);
    assert_eq!(motions[5]["file"], "motions/haru_g_m09.motion3.json");
    runtime.quit();
}

#[test]
fn optional_haru_get_expressions_lists_declared_names_in_order() {
    let Some(model) = fixture() else { return };
    let runtime = Runtime::start("--model", &model);
    assert_eq!(
        runtime.request(json!({"command":"get_expressions"})),
        json!({"ok":true,"expressions":["F01","F02","F03","F04","F05","F06","F07","F08"]})
    );
    runtime.quit();
}

#[test]
fn optional_haru_get_hotkeys_without_configuration_is_ok_and_empty() {
    let Some(model) = fixture() else { return };
    let runtime = Runtime::start("--model", &model);
    let response = runtime.request(json!({"command":"get_hotkeys"}));
    assert_eq!(response["ok"], true);
    assert_eq!(response["hotkeys"], json!([]));
    assert_eq!(response["warning"], "");
    // A fresh load still issues a token, so clients always have one to present.
    assert!(!response["model_token"].as_str().unwrap().is_empty());
    runtime.quit();
}

#[test]
fn optional_haru_get_hotkeys_reports_reasons_and_triggerable_bindings() {
    let Some(model) = fixture() else { return };
    let dir = tempdir().unwrap();
    let path = add_model(dir.path(), "Hotkeys", &model);
    let model_dir = path.parent().unwrap();
    write_expression(model_dir, "wave.exp3.json", "ParamAngleX");
    write_vtube_config(
        model_dir,
        json!([
            {
                "HotkeyID": "wave",
                "Action": "ToggleExpression",
                "File": "wave.exp3.json",
                "IsActive": true,
                "Triggers": {"Trigger1": "Tab"},
                "FadeSecondsAmount": 0.5,
            },
            // Same ID as the first binding, so both are flagged.
            {"HotkeyID": "wave", "Action": "ToggleExpression", "File": "wave.exp3.json"},
            {"HotkeyID": "move", "Action": "MoveModel", "File": "wave.exp3.json"},
            {"HotkeyID": "gone", "Action": "ToggleExpression", "File": "missing.exp3.json"},
        ]),
    );
    let runtime = Runtime::start("--models-dir", dir.path());
    let response = runtime.request(json!({"command":"get_hotkeys"}));
    assert_eq!(response["ok"], true);
    assert_eq!(response["warning"], "");
    let token = response["model_token"].as_str().unwrap().to_string();

    let entries = response["hotkeys"].as_array().unwrap();
    assert_eq!(entries.len(), 4);
    // The valid binding resolves through the `Expressions/` fallback directory.
    assert_eq!(
        entries[0],
        json!({
            "id": "wave",
            "name": "wave.exp3.json",
            "action": "ToggleExpression",
            "file": "wave.exp3.json",
            "reason": "",
            "enabled": true,
            "active": false,
            "keys": ["Tab"],
        })
    );
    assert_eq!(entries[0]["reason"], "");
    // A duplicate ID, an unsupported action and a missing file each carry the
    // reason the C++ records, and none of them can be triggered.
    let duplicate = &entries[1];
    assert_eq!(duplicate["reason"], "Missing or duplicate hotkey ID");
    assert_eq!(duplicate["active"], false);
    for (entry, reason) in [
        (&entries[2], "Unsupported action"),
        (&entries[3], "Expression file missing or invalid"),
    ] {
        assert_eq!(entry["reason"], reason);
        assert_eq!(entry["active"], false);
    }
    // The resolvable binding is triggerable despite the unusable entries.
    let triggered = runtime
        .request(json!({"command":"trigger_hotkey","args":{"id":"wave","model_token":token}}));
    assert_eq!(triggered["ok"], true);
    assert_eq!(hotkey_active(&triggered, "wave"), Some(true));
    for (id, reason) in [
        ("move", "Unsupported action"),
        ("gone", "Expression file missing or invalid"),
    ] {
        let reply = runtime
            .request(json!({"command":"trigger_hotkey","args":{"id":id,"model_token":token}}));
        assert_eq!(reply["ok"], false, "{id}");
        assert_eq!(reply["error"], reason, "{id}");
    }
    runtime.quit();
}

#[test]
fn optional_haru_trigger_hotkey_toggles_tracks_staleness_and_survives_switches() {
    let Some(model) = fixture() else { return };
    let dir = tempdir().unwrap();
    let first = add_model(dir.path(), "Alpha", &model);
    let first_dir = first.parent().unwrap();
    write_expression(first_dir, "wave.exp3.json", "ParamAngleX");
    write_vtube_config(
        first_dir,
        json!([{
            "HotkeyID": "wave",
            "Action": "ToggleExpression",
            "File": "wave.exp3.json",
            "IsActive": true,
        }]),
    );
    let second = add_model(dir.path(), "Beta", &model);
    let second_dir = second.parent().unwrap();
    write_expression(second_dir, "blush.exp3.json", "ParamAngleY");
    write_vtube_config(
        second_dir,
        json!([{
            "HotkeyID": "blush",
            "Action": "ToggleExpression",
            "File": "blush.exp3.json",
            "IsActive": true,
        }]),
    );
    let runtime = Runtime::start("--models-dir", dir.path());
    let reply = runtime.request(json!({"command":"get_hotkeys"}));
    let token = reply["model_token"].as_str().unwrap().to_string();
    assert_eq!(hotkey_active(&reply, "wave"), Some(false));

    let trigger = |id: &str, token: &str| {
        runtime.request(json!({"command":"trigger_hotkey","args":{"id":id,"model_token":token}}))
    };
    // Two successful triggers toggle the expression on and back off; the reply is
    // the same metadata object `get_hotkeys` returns.
    let on = trigger("wave", &token);
    assert_eq!(on["ok"], true);
    assert_eq!(on["model_token"], token);
    assert_eq!(hotkey_active(&on, "wave"), Some(true));
    assert_eq!(
        hotkey_active(&runtime.request(json!({"command":"get_hotkeys"})), "wave"),
        Some(true)
    );
    let off = trigger("wave", &token);
    assert_eq!(off["ok"], true);
    assert_eq!(hotkey_active(&off, "wave"), Some(false));

    // Unknown ids, stale tokens and missing tokens all fail without toggling.
    let unknown = trigger("nope", &token);
    assert_eq!(unknown["ok"], false);
    assert_eq!(
        unknown["error"],
        "Hotkey not found (the model may have changed)"
    );
    // A missing `id` is not a hotkey of this model either; the C++ reads it as
    // the empty string and `Trigger("")` reports the same not-found text.
    for args in [
        json!({"model_token":&token}),
        json!({"id":7,"model_token":&token}),
    ] {
        let reply = runtime.request(json!({"command":"trigger_hotkey","args":args}));
        assert_eq!(reply["ok"], false, "{args}");
        assert_eq!(
            reply["error"], "Hotkey not found (the model may have changed)",
            "{args}"
        );
    }
    for args in [
        json!({"id":"wave","model_token":"stale"}),
        json!({"id":"wave"}),
        json!({"id":"wave","model_token":""}),
    ] {
        let reply = runtime.request(json!({"command":"trigger_hotkey","args":args}));
        assert_eq!(reply["ok"], false, "{args}");
        assert_eq!(
            reply["error"],
            "Model changed; refresh hotkeys before triggering"
        );
    }
    assert_eq!(
        hotkey_active(&runtime.request(json!({"command":"get_hotkeys"})), "wave"),
        Some(false)
    );

    // A real switch reloads the other model's hotkeys and issues a new token; the
    // expression active before the switch is not reported active afterwards.
    assert_eq!(trigger("wave", &token)["ok"], true);
    assert_eq!(
        runtime.request(json!({"command":"set_model","args":{"name":"Beta"}}))["ok"],
        true
    );
    let switched = runtime.request(json!({"command":"get_hotkeys"}));
    let switched_token = switched["model_token"].as_str().unwrap().to_string();
    assert_ne!(switched_token, token);
    assert_eq!(
        switched["hotkeys"]
            .as_array()
            .unwrap()
            .iter()
            .map(|entry| entry["id"].as_str().unwrap())
            .collect::<Vec<_>>(),
        ["blush"]
    );
    assert_eq!(hotkey_active(&switched, "blush"), Some(false));
    // The previous token is stale, and the freshly issued one works.
    assert_eq!(
        runtime
            .request(json!({"command":"trigger_hotkey","args":{"id":"blush","model_token":token}}))
            ["error"],
        "Model changed; refresh hotkeys before triggering"
    );
    assert_eq!(
        hotkey_active(&trigger("blush", &switched_token), "blush"),
        Some(true)
    );

    // Switching back is a fresh load again: a new token and no remembered state.
    assert_eq!(
        runtime.request(json!({"command":"set_model","args":{"name":"Alpha"}}))["ok"],
        true
    );
    let back = runtime.request(json!({"command":"get_hotkeys"}));
    assert_ne!(back["model_token"], switched_token);
    assert_eq!(hotkey_active(&back, "wave"), Some(false));
    runtime.quit();
}

#[test]
fn optional_haru_reselecting_the_current_model_keeps_the_hotkey_token() {
    let Some(model) = fixture() else { return };
    let runtime = Runtime::start("--model", &model);
    let token = runtime.request(json!({"command":"get_hotkeys"}))["model_token"].clone();
    // Re-selecting the current model is not a scene change in the C++, so the
    // token and the loaded hotkeys stay as they are.
    assert_eq!(runtime.request(json!({"command":"next_model"}))["ok"], true);
    assert_eq!(
        runtime.request(json!({"command":"get_hotkeys"}))["model_token"],
        token
    );
    runtime.quit();
}

#[test]
fn optional_haru_trigger_hotkey_deactivates_after_the_configured_delay() {
    let Some(model) = fixture() else { return };
    let dir = tempdir().unwrap();
    let path = add_model(dir.path(), "Timed", &model);
    let model_dir = path.parent().unwrap();
    write_expression(model_dir, "timed.exp3.json", "ParamAngleX");
    write_vtube_config(
        model_dir,
        json!([{
            "HotkeyID": "timed",
            "Action": "ToggleExpression",
            "File": "timed.exp3.json",
            "IsActive": true,
            "DeactivateAfterSeconds": true,
            "DeactivateAfterSecondsAmount": 0.2,
            // Instant fade, so the weight is not what decides the active flag.
            "FadeSecondsAmount": 0.0,
        }]),
    );
    let runtime = Runtime::start("--models-dir", dir.path());
    let token = runtime.request(json!({"command":"get_hotkeys"}))["model_token"].clone();
    let triggered = runtime
        .request(json!({"command":"trigger_hotkey","args":{"id":"timed","model_token":token}}));
    assert_eq!(hotkey_active(&triggered, "timed"), Some(true));
    // The 16 ms loop drives `Hotkeys::update`, so the timeout expires on its own.
    // Poll with a bounded wait instead of sleeping a fixed time.
    let deadline = Instant::now() + TIMEOUT;
    loop {
        let reply = runtime.request(json!({"command":"get_hotkeys"}));
        if hotkey_active(&reply, "timed") == Some(false) {
            break;
        }
        assert!(
            Instant::now() < deadline,
            "hotkey did not time out: {reply}"
        );
        std::thread::sleep(Duration::from_millis(10));
    }
    runtime.quit();
}

#[test]
fn optional_haru_set_expression_and_do_motion_refuse_without_a_renderer() {
    let Some(model) = fixture() else { return };
    let runtime = Runtime::start("--model", &model);
    for (command, args) in [
        ("set_expression", json!({"id":"F01"})),
        ("do_motion", json!({"group":"Idle","index":0})),
    ] {
        let reply = runtime.request(json!({"command":command,"args":args}));
        assert_eq!(reply["ok"], false, "{command}");
        assert!(
            reply["error"]
                .as_str()
                .unwrap()
                .contains("renderer not available"),
            "{command}: {reply}"
        );
    }
    runtime.quit();
}
