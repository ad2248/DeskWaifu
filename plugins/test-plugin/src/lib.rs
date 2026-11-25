//! 注册用于验证命令系统的 `test` 子命令。
use deskwaifu_sdk::{Context, Plugin, PluginResult};

#[derive(Default)]
pub struct TestPlugin;

deskwaifu_sdk::export_plugin!(TestPlugin::new());

impl TestPlugin {
    pub fn new() -> Self {
        Self
    }
}

impl Plugin for TestPlugin {
    fn plugin_id(&self) -> &str {
        "test-plugin"
    }

    fn initialize(&mut self, cx: &mut Context<'_>) -> PluginResult {
        cx.register_command(
            "test",
            vec![
                "Run the test command.".into(),
                "All following input is passed through as string arguments.".into(),
            ],
        )
    }

    fn on_command(
        &mut self,
        name: &str,
        args: &[String],
        _cx: &mut Context<'_>,
    ) -> Result<String, String> {
        match name {
            "test" => Ok(format!(
                "test command received {} argument(s): {:?}",
                args.len(),
                args
            )),
            _ => Err(format!("unhandled command: {name}")),
        }
    }

    fn on_message(
        &mut self,
        _message: &deskwaifu_sdk::Message,
        _cx: &mut Context<'_>,
    ) -> PluginResult {
        Ok(())
    }
}
