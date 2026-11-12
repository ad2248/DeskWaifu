use serde_json::Value;
use std::fs;
use std::path::PathBuf;
use std::process::Command;

#[test]
fn rejects_invalid_arguments_and_model_file() {
    let binary = env!("CARGO_BIN_EXE_deskwaifu-rust");
    let output = Command::new(binary)
        .args(["--socket", "/tmp/desk-no-socket"])
        .output()
        .unwrap();
    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains("exactly one"));

    let dir = std::env::temp_dir().join(format!("desk-invalid-model-{}", std::process::id()));
    fs::create_dir_all(&dir).unwrap();
    let model = dir.join("Bad.model3.json");
    fs::write(&model, "{}").unwrap();
    let output = Command::new(binary)
        .args([
            "--model",
            model.to_str().unwrap(),
            "--socket",
            dir.join("socket").to_str().unwrap(),
        ])
        .output()
        .unwrap();
    assert!(!output.status.success());
    assert!(String::from_utf8_lossy(&output.stderr).contains("invalid model3.json"));
    fs::remove_dir_all(dir).unwrap();
}

#[test]
fn optional_external_haru_fixture_starts_real_core_and_quits_over_ipc() {
    let Ok(model) = std::env::var("CUBISM_TEST_MODEL") else {
        return;
    };
    let model = PathBuf::from(model);
    let dir = std::env::temp_dir().join(format!("desk-runtime-{}", std::process::id()));
    fs::create_dir_all(&dir).unwrap();
    let socket = dir.join("runtime.sock");
    let mut child = Command::new(env!("CARGO_BIN_EXE_deskwaifu-rust"))
        .args([
            "--model",
            model.to_str().unwrap(),
            "--socket",
            socket.to_str().unwrap(),
        ])
        .spawn()
        .unwrap();
    let mut connected = false;
    for _ in 0..100 {
        if std::os::unix::net::UnixStream::connect(&socket).is_ok() {
            connected = true;
            break;
        }
        if child.try_wait().unwrap().is_some() {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(20));
    }
    assert!(
        connected,
        "runtime failed to bind; CUBISM_SDK_ROOT must point to an installed SDK"
    );
    let mut stream = std::os::unix::net::UnixStream::connect(&socket).unwrap();
    use std::io::{BufRead, Write};
    stream.write_all(b"{\"command\":\"get_status\"}\n").unwrap();
    let mut response = Vec::new();
    std::io::BufReader::new(&mut stream)
        .read_until(b'\n', &mut response)
        .unwrap();
    let status: Value = serde_json::from_slice(&response).unwrap();
    assert_eq!(status["renderer_available"], false);
    assert_eq!(status["model_core_ready"], true);
    stream
        .write_all(b"{\"command\":\"get_parameters\"}\n")
        .unwrap();
    response.clear();
    std::io::BufReader::new(&mut stream)
        .read_until(b'\n', &mut response)
        .unwrap();
    let parameters: Value = serde_json::from_slice(&response).unwrap();
    assert!(parameters["parameters"].is_array());
    drop(stream);
    let mut stream = std::os::unix::net::UnixStream::connect(&socket).unwrap();
    stream.write_all(b"{\"command\":\"quit\"}\n").unwrap();
    response.clear();
    std::io::BufReader::new(&mut stream)
        .read_until(b'\n', &mut response)
        .unwrap();
    assert_eq!(
        serde_json::from_slice::<Value>(&response).unwrap()["ok"],
        true
    );
    drop(stream);
    assert!(child.wait().unwrap().success());
    assert!(!socket.exists());
    fs::remove_dir_all(dir).unwrap();
}
