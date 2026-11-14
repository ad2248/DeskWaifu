//! 注册用于验证命令系统的 `test` 子命令。
use deskwaifu_sdk::{Context, Plugin, PluginResult};

pub struct TestPlugin;

impl TestPlugin {
    pub fn new() -> Self {
        Self
    }
}

fn test_command(args: &[String]) -> Result<String, String> {
    Ok(format!(
        "test command received {} argument(s): {:?}",
        args.len(),
        args
    ))
}

impl Plugin for TestPlugin {
    fn name(&self) -> &str {
        "test-plugin"
    }

    fn initialize(&mut self, cx: &mut Context<'_>) -> PluginResult {
        cx.register_command(
            "test",
            vec![
                "Run the test command.".into(),
                "All following input is passed through as string arguments.".into(),
            ],
            test_command,
        )
    }

    fn on_message(
        &mut self,
        _message: &deskwaifu_sdk::Message,
        _cx: &mut Context<'_>,
    ) -> PluginResult {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use deskwaifu_runtime::Runtime;

    #[test]
    fn registers_test_command_and_preserves_arguments() {
        let mut runtime = Runtime::new(16);
        runtime.add(TestPlugin::new()).unwrap();
        runtime.start().unwrap();
        assert_eq!(
            runtime.dispatch_command(&["test".into(), "hello world".into(), "001".into()]),
            Ok("test command received 2 argument(s): [\"hello world\", \"001\"]".into())
        );
        let help = runtime.dispatch_command(&["help".into()]).unwrap();
        assert!(help.contains("help  Show available commands"));
        assert!(help.contains("test  Run the test command."));
        assert!(help.contains("All following input is passed through as string arguments."));
        runtime.stop().unwrap();
    }

    #[test]
    fn no_test_messages_are_sent() {
        let mut runtime = Runtime::new(16);
        runtime.add(TestPlugin::new()).unwrap();
        runtime.start().unwrap();
        runtime.advance(std::time::Duration::from_secs(30)).unwrap();
        runtime.stop().unwrap();
    }
}
