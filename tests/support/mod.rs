use std::{
    fs,
    path::PathBuf,
    process::Command,
    sync::atomic::{AtomicUsize, Ordering},
};

/// Build the real plugin independently, without linking it into the test host.
/// A separate target directory avoids nested Cargo lock contention and stale artifacts.
pub struct PluginBuild {
    pub target: PathBuf,
}

impl PluginBuild {
    pub fn new() -> (Self, Vec<u8>) {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        fs::create_dir_all("/tmp/opencode").unwrap();
        let target = loop {
            let path = PathBuf::from(format!(
                "/tmp/opencode/deskwaifu-dynamic-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::Relaxed)
            ));
            match fs::create_dir(&path) {
                Ok(()) => break path,
                Err(error) if error.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(error) => panic!("create independent build directory: {error}"),
            }
        };
        let build = Self { target };
        let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..");
        let output = Command::new(env!("CARGO"))
            .current_dir(root)
            .args([
                "build",
                "--message-format=json",
                "--offline",
                "--locked",
                "-p",
                "deskwaifu-test-plugin",
                "--target-dir",
            ])
            .arg(&build.target)
            .output()
            .unwrap();
        assert!(
            output.status.success(),
            "plugin build failed: {}",
            String::from_utf8_lossy(&output.stderr)
        );
        assert!(build.plugin().is_file());
        (build, output.stdout)
    }

    pub fn plugin(&self) -> PathBuf {
        self.target.join("debug/libdeskwaifu_test_plugin.so")
    }
}

impl Drop for PluginBuild {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.target);
    }
}
