use super::*;

#[test]
fn plugins_can_read_and_write_only_their_own_section() {
    // 两个插件通过同一接口分别读写，重读文件验证节点独立且持久化。
    struct SettingsPlugin(&'static str);
    impl Plugin for SettingsPlugin {
        fn plugin_id(&self) -> &str {
            self.0
        }
        fn initialize(&mut self, cx: &mut Context<'_>) -> PluginResult {
            cx.register_command(self.0, vec![])
        }
        fn on_message(&mut self, _: &Message, _: &mut Context<'_>) -> PluginResult {
            Ok(())
        }
        fn on_command(
            &mut self,
            _: &str,
            args: &[String],
            cx: &mut Context<'_>,
        ) -> Result<String, String> {
            if let Some(value) = args.first() {
                cx.replace_config(serde_json::json!({"value": value}))?;
            }
            Ok(cx.config().unwrap_or_default().to_string())
        }
    }
    let temp = tempfile::tempdir().unwrap();
    let path = temp.path().join("config.jsonc");
    let mut runtime = Runtime::new(8);
    runtime.load_config(path.clone()).unwrap();
    runtime.add(SettingsPlugin("terminal")).unwrap();
    runtime.add(SettingsPlugin("voice_input")).unwrap();
    runtime.start().unwrap();
    runtime
        .dispatch_command(&["terminal".into(), "pet".into()])
        .unwrap();
    runtime
        .dispatch_command(&["voice_input".into(), "audio".into()])
        .unwrap();
    assert_eq!(
        runtime.dispatch_command(&["terminal".into()]).unwrap(),
        r#"{"value":"pet"}"#
    );
    let stored = ConfigStore::load(path).unwrap();
    assert_eq!(stored.section("terminal").unwrap()["value"], "pet");
    assert_eq!(stored.section("voice_input").unwrap()["value"], "audio");
}
