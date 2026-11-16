//! Trusted same-toolchain Rust ABI fixture, compiled independently by dynamic.rs.
use deskwaifu_sdk::{Context, Message, Plugin, PluginResult};
use std::{fs::OpenOptions, io::Write};

fn record(event: &str) {
    writeln!(
        OpenOptions::new()
            .create(true)
            .append(true)
            .open(env!("FIXTURE_LOG"))
            .unwrap(),
        "{}:{event}",
        env!("FIXTURE_NAME")
    )
    .unwrap();
}

struct Lifecycle;

fn create() -> Lifecycle {
    record("create");
    Lifecycle
}

deskwaifu_sdk::export_plugin!(create());

impl Plugin for Lifecycle {
    fn name(&self) -> &str {
        env!("FIXTURE_NAME")
    }

    fn initialize(&mut self, cx: &mut Context<'_>) -> PluginResult {
        record("initialize");
        cx.register_command(
            env!("FIXTURE_COMMAND"),
            vec!["Fixture command".into()],
            |_| Ok("fixture callback".into()),
        )?;
        if cfg!(fail_init) {
            Err("fixture initialization failed".into())
        } else {
            Ok(())
        }
    }

    fn start(&mut self, _cx: &mut Context<'_>) -> PluginResult {
        record("start");
        Ok(())
    }

    fn on_message(&mut self, _message: &Message, _cx: &mut Context<'_>) -> PluginResult {
        Ok(())
    }

    fn stop(&mut self, _cx: &mut Context<'_>) -> PluginResult {
        record("stop");
        Ok(())
    }
}

impl Drop for Lifecycle {
    fn drop(&mut self) {
        record("destroy");
    }
}

// Linux/ELF runs this callback on dlclose. This is an internal loader hook,
// not a C ABI plugin entry point; the plugin entry above uses the Rust ABI.
#[cfg(target_os = "linux")]
extern "C" fn unloaded() {
    record("unload");
}

#[cfg(target_os = "linux")]
#[used]
#[allow(unsafe_code)] // Trusted fixture: place a valid callback in ELF's finalizer array.
#[unsafe(link_section = ".fini_array")]
static FINI: extern "C" fn() = unloaded;
