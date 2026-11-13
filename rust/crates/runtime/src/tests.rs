use super::*;
struct Timer;
impl Plugin for Timer {
    fn name(&self) -> &str {
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
fn full_queue_still_accepts_unsubscribed_types() {
    let mut bus = Bus {
        active: true,
        capacity: 1,
        subscriptions: vec![
            HashSet::from([TypeId::of::<u32>()]),
            HashSet::from([TypeId::of::<u64>()]),
        ],
        queue: VecDeque::new(),
    };
    {
        let mut host = ScopedHost {
            bus: &mut bus,
            id: PluginId(0),
            initializing: false,
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
    };
    assert_eq!(
        host.publish(Message::new(PluginId(0), 1_u32)),
        Err(PublishError::QueueFull)
    );
}
