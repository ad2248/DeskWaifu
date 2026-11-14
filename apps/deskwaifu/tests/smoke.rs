use std::{
    process::Command,
    time::{Duration, Instant},
};

#[test]
fn demo_runs_periodically_and_verifies_self_delivery() {
    let started = Instant::now();
    let output = Command::new(env!("CARGO_BIN_EXE_deskwaifu"))
        .output()
        .unwrap();
    assert!(
        output.status.success(),
        "stderr: {}",
        String::from_utf8_lossy(&output.stderr)
    );
    assert!(started.elapsed() >= Duration::from_secs(3));
    let stdout = String::from_utf8(output.stdout).unwrap();
    assert!(stdout.contains("[test-plugin] sent #1"), "{stdout}");
    assert!(stdout.contains("[test-plugin] received own #1"), "{stdout}");
    assert!(stdout.contains("self-message check passed:"), "{stdout}");
}
