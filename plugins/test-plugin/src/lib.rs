//! 每秒发布一次消息，并核验自己的订阅投递链路。
use deskwaifu_sdk::{Context, Message, Plugin, PluginResult};
use std::{cell::Cell, rc::Rc, time::Duration};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Counts {
    pub sent: u64,
    pub received: u64,
}

/// 只读观测句柄，不依赖 runtime 的具体实现。
#[derive(Clone)]
pub struct Report(Rc<Cell<Counts>>);
impl Report {
    pub fn counts(&self) -> Counts {
        self.0.get()
    }
}

#[derive(Debug)]
struct TestMessage {
    sequence: u64,
}

pub struct TestPlugin {
    report: Report,
}
impl TestPlugin {
    pub fn new() -> (Self, Report) {
        let report = Report(Rc::new(Cell::new(Counts::default())));
        (
            Self {
                report: report.clone(),
            },
            report,
        )
    }

    fn check_received(&self) -> PluginResult {
        let counts = self.report.counts();
        if counts.sent != counts.received {
            return Err(format!(
                "missing self-message: sent={}, received={}",
                counts.sent, counts.received
            ));
        }
        Ok(())
    }
}

impl Plugin for TestPlugin {
    fn name(&self) -> &str {
        "test-plugin"
    }

    fn initialize(&mut self, cx: &mut Context<'_>) -> PluginResult {
        cx.subscribe::<TestMessage>()
    }

    fn tick_interval(&self) -> Option<Duration> {
        Some(Duration::from_secs(1))
    }

    fn tick(&mut self, cx: &mut Context<'_>) -> PluginResult {
        self.check_received()?;
        let mut counts = self.report.counts();
        let sequence = counts.sent + 1;
        cx.publish(TestMessage { sequence })
            .map_err(|e| e.to_string())?;
        counts.sent = sequence;
        self.report.0.set(counts);
        println!("[test-plugin] sent #{sequence}");
        Ok(())
    }

    fn on_message(&mut self, message: &Message, cx: &mut Context<'_>) -> PluginResult {
        if message.sender != cx.id() {
            return Ok(());
        }
        let payload = message
            .payload::<TestMessage>()
            .ok_or("unexpected message type")?;
        let mut counts = self.report.counts();
        if payload.sequence != counts.received + 1 || payload.sequence > counts.sent {
            return Err(format!("unexpected sequence: {}", payload.sequence));
        }
        counts.received += 1;
        self.report.0.set(counts);
        println!("[test-plugin] received own #{}", payload.sequence);
        Ok(())
    }

    fn stop(&mut self, _cx: &mut Context<'_>) -> PluginResult {
        self.check_received()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use deskwaifu_runtime::Runtime;

    #[test]
    fn publishes_once_each_second_and_receives_own_message() {
        let (plugin, report) = TestPlugin::new();
        let mut runtime = Runtime::new(16);
        runtime.add(plugin).unwrap();
        runtime.start().unwrap();
        runtime.advance(Duration::from_millis(999)).unwrap();
        assert_eq!(report.counts(), Counts::default());
        runtime.advance(Duration::from_millis(1)).unwrap();
        assert_eq!(
            report.counts(),
            Counts {
                sent: 1,
                received: 1
            }
        );
        for expected in 2..=3 {
            runtime.advance(Duration::from_secs(1)).unwrap();
            assert_eq!(
                report.counts(),
                Counts {
                    sent: expected,
                    received: expected
                }
            );
        }
        runtime.stop().unwrap();
        assert!(runtime.advance(Duration::from_secs(1)).is_err());
        assert_eq!(
            report.counts(),
            Counts {
                sent: 3,
                received: 3
            }
        );
    }

    #[test]
    fn delayed_tick_does_not_burst_old_messages() {
        let (plugin, report) = TestPlugin::new();
        let mut runtime = Runtime::new(16);
        runtime.add(plugin).unwrap();
        runtime.start().unwrap();
        runtime.advance(Duration::from_secs(10)).unwrap();
        assert_eq!(
            report.counts(),
            Counts {
                sent: 1,
                received: 1
            }
        );
        runtime.stop().unwrap();
    }
}
