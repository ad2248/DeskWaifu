//! Trusted same-toolchain Rust ABI fixture, compiled independently by dynamic.rs.
use deskwaifu_sdk::{Context, Message, Plugin, PluginResult};
use std::{fs::OpenOptions, future::pending, io::Write};

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

struct TaskGuard;

impl Drop for TaskGuard {
    fn drop(&mut self) {
        record("task-drop");
    }
}

fn create() -> Lifecycle {
    record("create");
    Lifecycle
}

deskwaifu_sdk::export_plugin!(create());

impl Plugin for Lifecycle {
    fn plugin_id(&self) -> &str {
        env!("FIXTURE_NAME")
    }

    fn initialize(&mut self, cx: &mut Context<'_>) -> PluginResult {
        record("initialize");
        cx.register_command(env!("FIXTURE_COMMAND"), vec!["Fixture command".into()])?;
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

    fn on_command(
        &mut self,
        name: &str,
        args: &[String],
        cx: &mut Context<'_>,
    ) -> Result<String, String> {
        assert_eq!(name, env!("FIXTURE_COMMAND"));
        if args.first().is_some_and(|arg| arg == "spawn") {
            let guard = TaskGuard;
            cx.spawn_local(async move {
                record("task-poll");
                pending::<()>().await;
                drop(guard);
            })?;
        }
        Ok("fixture callback".into())
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
