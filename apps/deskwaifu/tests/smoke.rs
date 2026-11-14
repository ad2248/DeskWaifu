use std::process::Command;

#[test]
fn default_invocation_prints_registered_commands() {
    let output = Command::new(env!("CARGO_BIN_EXE_deskwaifu"))
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "stderr: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    let stdout = String::from_utf8(output.stdout).unwrap();
    assert!(stdout.contains("help  Show available commands"), "{stdout}");
    assert!(stdout.contains("test  Run the test command."), "{stdout}");
}
