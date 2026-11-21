use super::*;

#[test]
fn plugin_ids_reject_uuid_and_invalid_names() {
    // 验证英文标识可以使用下划线，而 UUID 和带空格的标识会在初始化前被拒绝。
    struct Named(&'static str);
    impl Plugin for Named {
        fn plugin_id(&self) -> &str {
            self.0
        }
        fn initialize(&mut self, _: &mut Context<'_>) -> PluginResult {
            Ok(())
        }
        fn on_message(&mut self, _: &Message, _: &mut Context<'_>) -> PluginResult {
            Ok(())
        }
    }
    let mut runtime = Runtime::new(8);
    runtime.add(Named("voice_input")).unwrap();
    for id in [
        "voice_input",
        "a7225741-8c45-4e9c-8e10-560eabd69626",
        "Voice Input",
        "",
    ] {
        assert!(runtime.add(Named(id)).is_err(), "{id:?}");
    }
}
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

struct Timer;
impl Plugin for Timer {
    fn plugin_id(&self) -> &str {
        "timer"
    }
    fn initialize(&mut self, _: &mut Context<'_>) -> PluginResult {
        Ok(())
    }
    fn tick_interval(&self) -> Option<Duration> {
        Some(Duration::from_secs(1))
    }
    fn on_message(&mut self, _: &Message, _: &mut Context<'_>) -> PluginResult {
        Ok(())
    }
}

#[test]
fn sleep_deducts_callback_time_and_saturates_when_overdue() {
    let mut runtime = Runtime::new(8);
    runtime.add(Timer).unwrap();
    runtime.start().unwrap();
    runtime.advance(Duration::from_secs(1)).unwrap();
    assert_eq!(
        runtime.wait_duration(Duration::from_millis(200), Duration::from_secs(5)),
        Duration::from_millis(800)
    );
    assert_eq!(
        runtime.wait_duration(Duration::from_secs(2), Duration::from_secs(5)),
        Duration::ZERO
    );
    assert_eq!(
        runtime.wait_duration(Duration::ZERO, Duration::from_millis(100)),
        Duration::from_millis(100)
    );
}

#[test]
fn next_wakeup_tracks_timers_messages_and_idle_state() {
    let mut idle = Runtime::new(8);
    assert_eq!(idle.next_wakeup(), None);
    idle.start().unwrap();
    assert_eq!(idle.next_wakeup(), None);

    let mut runtime = Runtime::new(8);
    runtime.add(Timer).unwrap();
    runtime.start().unwrap();
    assert_eq!(runtime.next_wakeup(), Some(Duration::from_secs(1)));
    runtime.advance(Duration::from_millis(400)).unwrap();
    assert_eq!(runtime.next_wakeup(), Some(Duration::from_millis(600)));
    runtime.bus.queue.push_back(Delivery {
        message: Message::new(PluginId(0), 1_u32),
        targets: vec![],
    });
    assert_eq!(runtime.next_wakeup(), Some(Duration::ZERO));
    runtime.advance(Duration::from_millis(600)).unwrap();
    assert_eq!(runtime.next_wakeup(), Some(Duration::from_secs(1)));
    runtime.stop().unwrap();
    assert_eq!(runtime.next_wakeup(), None);
}

#[test]
fn full_queue_still_accepts_unsubscribed_types() {
    let mut bus = Bus {
        os: None,
        active: true,
        capacity: 1,
        subscriptions: vec![
            HashSet::from([TypeId::of::<u32>()]),
            HashSet::from([TypeId::of::<u64>()]),
        ],
        queue: VecDeque::new(),
        tasks: FuturesUnordered::new(),
        panel_tabs: Vec::new(),
        plugin_ids: Vec::new(),
        config: None,
    };
    {
        let mut host = ScopedHost {
            bus: &mut bus,
            id: PluginId(0),
            initializing: false,
            commands: &mut BTreeMap::new(),
        };
        host.publish(Message::new(PluginId(0), 1_u32)).unwrap();
        host.publish(Message::new(PluginId(0), "unsubscribed"))
            .unwrap();
        assert_eq!(
            host.publish(Message::new(PluginId(0), 2_u32)),
            Err(PublishError::QueueFull)
        );
    }
    assert_eq!(bus.queue.front().unwrap().targets, vec![0]);
    bus.capacity = 0;
    bus.queue.clear();
    let mut host = ScopedHost {
        bus: &mut bus,
        id: PluginId(0),
        initializing: false,
        commands: &mut BTreeMap::new(),
    };
    assert_eq!(
        host.publish(Message::new(PluginId(0), 1_u32)),
        Err(PublishError::QueueFull)
    );
}
