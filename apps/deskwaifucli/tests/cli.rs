use std::process::Command;

#[test]
fn cli_help_runs_without_loading_plugins_or_starting_host() {
    for args in [vec![], vec!["--help"], vec!["-h"]] {
        let output = Command::new(env!("CARGO_BIN_EXE_deskwaifucli"))
            .args(args)
            .env_remove("XDG_RUNTIME_DIR")
            .output()
            .unwrap();
        assert!(output.status.success());
        let stdout = String::from_utf8(output.stdout).unwrap();
        assert!(stdout.contains("deskwaifucli test [ARGS...]"));
        assert!(stdout.contains("/tmp/deskwaifu.socket"));
        assert!(output.stderr.is_empty());
    }
}
