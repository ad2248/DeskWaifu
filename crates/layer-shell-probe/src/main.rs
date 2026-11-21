fn main() -> std::process::ExitCode {
    #[cfg(target_os = "linux")]
    let result = layer_shell_probe::run();
    #[cfg(not(target_os = "linux"))]
    let result: Result<(), &str> = Err("a Linux Wayland session is required");

    match result {
        Ok(()) => std::process::ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("layer-shell-probe: {error}");
            std::process::ExitCode::FAILURE
        }
    }
}
