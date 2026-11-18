//! Dynamic plugins deliberately share the repository's Rust ABI, not a stable C ABI.
// These tests use Linux .so libraries and ELF .fini_array unload observations.
#![cfg(target_os = "linux")]

#[path = "../../../tests/support/mod.rs"]
mod support;

use deskwaifu_runtime::Runtime;
use deskwaifu_sdk::PluginId;
use std::{fs, path::Path, process::Command};

fn compile_fixture(
    build: &support::PluginBuild,
    name: &str,
    command: &str,
    fail_init: bool,
) -> std::path::PathBuf {
    let deps = build.target.join("debug/deps");
    let sdk = fs::read_dir(&deps)
        .unwrap()
        .map(|entry| entry.unwrap().path())
        .find(|path| {
            path.file_name()
                .unwrap()
                .to_string_lossy()
                .starts_with("libdeskwaifu_sdk-")
                && path
                    .extension()
                    .is_some_and(|extension| extension == "rlib")
        })
        .expect("independently built SDK rlib");
    let output = build.target.join(format!("{name}.so"));
    let source = if name == "missing_entry" {
        "missing_entry.rs"
    } else {
        "lifecycle.rs"
    };
    let mut rustc = Command::new(std::env::var_os("RUSTC").unwrap_or_else(|| "rustc".into()));
    rustc
        .args(["--edition=2024", "--crate-type=cdylib", "--crate-name"])
        .arg(name)
        .arg(
            Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("tests/fixtures")
                .join(source),
        )
        .arg("--extern")
        .arg(format!("deskwaifu_sdk={}", sdk.display()))
        .arg("-L")
        .arg(format!("dependency={}", deps.display()))
        .arg("-o")
        .arg(&output)
        .env("FIXTURE_NAME", name)
        .env("FIXTURE_COMMAND", command)
        .env("FIXTURE_LOG", build.target.join("lifecycle.log"));
    if fail_init {
        rustc.args(["--cfg", "fail_init"]);
    }
    let result = rustc.output().unwrap();
    assert!(
        result.status.success(),
        "fixture {name} compilation failed: {}",
        String::from_utf8_lossy(&result.stderr)
    );
    output
}

fn events(build: &support::PluginBuild) -> Vec<String> {
    fs::read_to_string(build.target.join("lifecycle.log"))
        .unwrap_or_default()
        .lines()
        .map(str::to_owned)
        .collect()
}

fn clear_events(build: &support::PluginBuild) {
    fs::write(build.target.join("lifecycle.log"), "").unwrap();
}

#[test]
#[allow(unsafe_code)] // Only trusted, same-toolchain fixtures are loaded; malformed files cannot execute.
fn dynamic_plugins() {
    // Build the real cdylib once, separately from the host and its target directory.
    // Keep this guard alive until every runtime/library has been dropped.
    let build = support::PluginBuild::new();
    let plugin = build.plugin();
    let good = compile_fixture(&build, "good", "good", false);
    let failing = compile_fixture(&build, "failing", "failing", true);
    let conflicting = compile_fixture(&build, "conflicting", "good", false);
    let missing_entry = compile_fixture(&build, "missing_entry", "unused", false);
    let invalid = build.target.join("invalid.so");
    fs::write(&invalid, "not a shared library").unwrap();

    // A single-file loading error leaves Created intact and can be retried.
    {
        let mut runtime = Runtime::new(16);
        for path in [build.target.join("absent.so"), invalid.clone()] {
            let error = unsafe { runtime.load_plugin(&path) }.unwrap_err();
            assert!(error.contains("load plugin"), "{error}");
            assert!(error.contains(&path.display().to_string()), "{error}");
        }
        let error = unsafe { runtime.load_plugin(&missing_entry) }.unwrap_err();
        assert!(error.contains("plugin entry"), "{error}");
        assert!(error.contains("deskwaifu_plugin_create"), "{error}");
        assert_eq!(unsafe { runtime.load_plugin(&plugin) }, Ok(PluginId(0)));
        assert_eq!(
            unsafe { runtime.load_plugin(&plugin) },
            Err("duplicate plugin: test-plugin".into())
        );
        runtime.start().unwrap();
        let help = runtime.dispatch_command(&["help".into()]).unwrap();
        assert!(help.contains("help  Show available commands"));
        assert!(help.contains("test  Run the test command."));
        assert!(help.contains("All following input is passed through as string arguments."));
        let args: Vec<String> = ["hello world", "", "001", "--flag=a b", "中文", "\"quoted\""]
            .into_iter()
            .map(str::to_owned)
            .collect();
        let mut argv = vec!["test".into()];
        argv.extend(args.clone());
        assert_eq!(
            runtime.dispatch_command(&argv),
            Ok(format!(
                "test command received {} argument(s): {:?}",
                args.len(),
                args
            ))
        );
        assert_eq!(
            unsafe { runtime.load_plugin(&plugin) },
            Err("plugins must be loaded before start".into())
        );
        assert_eq!(
            unsafe { runtime.load_plugins_from(&build.target) },
            Err("plugins must be loaded before start".into())
        );
        runtime.stop().unwrap();
        runtime.stop().unwrap();
        assert!(runtime.dispatch_command(&argv).is_err());
        assert!(unsafe { runtime.load_plugin(&plugin) }.is_err());
        assert!(runtime.start().is_err());
    }

    let directory = build.target.join("scan");
    fs::create_dir(&directory).unwrap();
    {
        let mut runtime = Runtime::new(4);
        assert_eq!(unsafe { runtime.load_plugins_from(&directory) }, Ok(0));
        fs::write(directory.join("ignored.txt"), "not a library").unwrap();
        fs::write(directory.join("ignored.SO"), "case sensitive extension").unwrap();
        fs::create_dir(directory.join("subdirectory.so")).unwrap();
        assert_eq!(unsafe { runtime.load_plugins_from(&directory) }, Ok(0));
        runtime.start().unwrap();
    }

    // Create in reverse filename order: creation logs must still follow sorted paths.
    fs::copy(&good, directory.join("z-good.so")).unwrap();
    fs::copy(&failing, directory.join("a-failing.so")).unwrap();
    {
        let mut runtime = Runtime::new(4);
        assert_eq!(unsafe { runtime.load_plugins_from(&directory) }, Ok(2));
        assert_eq!(events(&build), ["failing:create", "good:create"]);
    }
    assert_eq!(
        events(&build),
        [
            "failing:create",
            "good:create",
            "failing:destroy",
            "good:destroy",
            "failing:unload",
            "good:unload"
        ]
    );
    clear_events(&build);

    // Explicit stop keeps the instance, callback and library alive until Runtime drop.
    {
        let mut runtime = Runtime::new(4);
        unsafe { runtime.load_plugin(&good) }.unwrap();
        runtime.start().unwrap();
        assert_eq!(
            runtime.dispatch_command(&["good".into()]),
            Ok("fixture callback".into())
        );
        runtime.stop().unwrap();
        runtime.stop().unwrap();
        assert_eq!(
            events(&build),
            ["good:create", "good:initialize", "good:start", "good:stop"]
        );
    }
    assert_eq!(
        events(&build),
        [
            "good:create",
            "good:initialize",
            "good:start",
            "good:stop",
            "good:destroy",
            "good:unload"
        ]
    );
    clear_events(&build);

    // Drop also stops a running plugin before destroying it and unloading its code.
    {
        let mut runtime = Runtime::new(4);
        unsafe { runtime.load_plugin(&good) }.unwrap();
        runtime.start().unwrap();
    }
    assert_eq!(
        events(&build),
        [
            "good:create",
            "good:initialize",
            "good:start",
            "good:stop",
            "good:destroy",
            "good:unload"
        ]
    );
    clear_events(&build);

    // A rejected duplicate is destroyed while its library (and the accepted instance) live.
    {
        let mut runtime = Runtime::new(4);
        unsafe { runtime.load_plugin(&good) }.unwrap();
        assert_eq!(
            unsafe { runtime.load_plugin(&good) },
            Err("duplicate plugin: good".into())
        );
        assert_eq!(
            events(&build),
            ["good:create", "good:create", "good:destroy"]
        );
    }
    assert_eq!(
        events(&build),
        [
            "good:create",
            "good:create",
            "good:destroy",
            "good:destroy",
            "good:unload"
        ]
    );
    clear_events(&build);

    // Failed initialization cleans up that plugin and leaves the healthy one running.
    {
        let mut runtime = Runtime::new(4);
        unsafe { runtime.load_plugin(&good) }.unwrap();
        unsafe { runtime.load_plugin(&failing) }.unwrap();
        runtime.start().unwrap();
        assert!(runtime.start().is_err());
        assert_eq!(
            runtime.dispatch_command(&["good".into()]),
            Ok("fixture callback".into())
        );
        assert_eq!(
            runtime.dispatch_command(&["failing".into()]),
            Err("unknown command: failing".into())
        );
        runtime.stop().unwrap();
        assert_eq!(
            events(&build),
            [
                "good:create",
                "failing:create",
                "good:initialize",
                "failing:initialize",
                "failing:stop",
                "failing:destroy",
                "good:start",
                "good:stop"
            ]
        );
    }
    assert_eq!(
        events(&build),
        [
            "good:create",
            "failing:create",
            "good:initialize",
            "failing:initialize",
            "failing:stop",
            "failing:destroy",
            "good:start",
            "good:stop",
            "good:destroy",
            "good:unload",
            "failing:unload"
        ]
    );
    clear_events(&build);

    // Distinct dynamic plugin names may load, but duplicate command registration
    // skips only the conflicting plugin and preserves the original command.
    {
        let mut runtime = Runtime::new(4);
        assert_eq!(unsafe { runtime.load_plugin(&good) }, Ok(PluginId(0)));
        assert_eq!(
            unsafe { runtime.load_plugin(&conflicting) },
            Ok(PluginId(1))
        );
        runtime.start().unwrap();
        assert!(runtime.start().is_err());
        assert_eq!(
            runtime.dispatch_command(&["good".into()]),
            Ok("fixture callback".into())
        );
        assert!(unsafe { runtime.load_plugin(&plugin) }.is_err());
        runtime.stop().unwrap();
        assert_eq!(
            events(&build),
            [
                "good:create",
                "conflicting:create",
                "good:initialize",
                "conflicting:initialize",
                "conflicting:stop",
                "conflicting:destroy",
                "good:start",
                "good:stop"
            ]
        );
    }
    assert_eq!(
        events(&build),
        [
            "good:create",
            "conflicting:create",
            "good:initialize",
            "conflicting:initialize",
            "conflicting:stop",
            "conflicting:destroy",
            "good:start",
            "good:stop",
            "good:destroy",
            "good:unload",
            "conflicting:unload"
        ]
    );
    clear_events(&build);

    // Directory failure closes the entire runtime, including already loaded entries.
    let broken = build.target.join("broken-scan");
    fs::create_dir(&broken).unwrap();
    fs::copy(&good, broken.join("a-good.so")).unwrap();
    fs::copy(&invalid, broken.join("z-invalid.so")).unwrap();
    {
        let mut runtime = Runtime::new(4);
        let error = unsafe { runtime.load_plugins_from(&broken) }.unwrap_err();
        assert!(error.contains("z-invalid.so"), "{error}");
        assert_eq!(events(&build), ["good:create"]);
        assert!(runtime.start().is_err());
        assert!(unsafe { runtime.load_plugin(&plugin) }.is_err());
        assert!(unsafe { runtime.load_plugins_from(&directory) }.is_err());
        runtime.stop().unwrap();
    }
    assert_eq!(
        events(&build),
        ["good:create", "good:destroy", "good:unload"]
    );

    for path in [build.target.join("absent-directory"), invalid] {
        let mut runtime = Runtime::new(4);
        assert!(unsafe { runtime.load_plugins_from(path) }.is_err());
        assert!(runtime.start().is_err());
    }
}
