use deskwaifu_runtime::Runtime;
use deskwaifu_sdk::{Context, Message, Plugin, PluginResult, PublishError};
use std::{cell::RefCell, rc::Rc, time::Duration};

type Log = Rc<RefCell<Vec<String>>>;
struct Probe {
    name: &'static str,
    log: Log,
    emit: bool,
    fail_initialize: bool,
    fail_message: bool,
    fail_stop: bool,
    fail_tick: bool,
    tick_count: Rc<RefCell<usize>>,
    interval: Option<Duration>,
    flood: bool,
}
impl Probe {
    fn new(name: &'static str, log: &Log) -> Self {
        Self {
            name,
            log: log.clone(),
            emit: false,
            fail_initialize: false,
            fail_message: false,
            fail_stop: false,
            fail_tick: false,
            tick_count: Rc::default(),
            interval: None,
            flood: false,
        }
    }
}
impl Plugin for Probe {
    fn name(&self) -> &str {
        self.name
    }
    fn initialize(&mut self, cx: &mut Context<'_>) -> PluginResult {
        self.log.borrow_mut().push(format!("{} init", self.name));
        assert_eq!(cx.publish(1_u32), Err(PublishError::Inactive));
        cx.subscribe::<u32>()?;
        cx.subscribe::<u32>()?;
        if self.fail_initialize {
            return Err("init failed".into());
        }
        Ok(())
    }
    fn start(&mut self, cx: &mut Context<'_>) -> PluginResult {
        self.log.borrow_mut().push(format!("{} start", self.name));
        assert!(cx.subscribe::<u64>().is_err());
        if self.emit {
            cx.publish(1_u32).map_err(|e| e.to_string())?;
        }
        Ok(())
    }
    fn tick_interval(&self) -> Option<Duration> {
        self.interval
    }
    fn tick(&mut self, _cx: &mut Context<'_>) -> PluginResult {
        *self.tick_count.borrow_mut() += 1;
        if self.fail_tick {
            return Err("tick failed".into());
        }
        Ok(())
    }
    fn on_message(&mut self, message: &Message, cx: &mut Context<'_>) -> PluginResult {
        let value = message.payload::<u32>().unwrap();
        assert!(message.payload::<u64>().is_none());
        self.log.borrow_mut().push(format!(
            "{} got {value} from {}",
            self.name, message.sender.0
        ));
        if self.fail_message {
            return Err("handler failed".into());
        }
        if self.flood || (self.emit && *value == 1) {
            cx.publish(value + 1).map_err(|e| e.to_string())?;
        }
        Ok(())
    }
    fn stop(&mut self, cx: &mut Context<'_>) -> PluginResult {
        self.log.borrow_mut().push(format!("{} stop", self.name));
        assert_eq!(cx.publish(3_u32), Err(PublishError::Inactive));
        if self.fail_stop {
            return Err("stop failed".into());
        }
        Ok(())
    }
}

#[test]
fn broadcast_includes_sender_and_is_fifo_non_recursive() {
    let log = Log::default();
    let mut runtime = Runtime::new(8);
    let mut a = Probe::new("a", &log);
    a.emit = true;
    runtime.add(a).unwrap();
    runtime.add(Probe::new("b", &log)).unwrap();
    runtime.start().unwrap();
    assert_eq!(&*log.borrow(), &["a init", "b init", "a start", "b start"]);
    runtime.advance(Duration::ZERO).unwrap();
    assert_eq!(
        &log.borrow()[4..],
        &[
            "a got 1 from 0",
            "b got 1 from 0",
            "a got 2 from 0",
            "b got 2 from 0"
        ]
    );
    runtime.stop().unwrap();
    assert_eq!(&log.borrow()[8..], &["b stop", "a stop"]);
    runtime.stop().unwrap();
    assert_eq!(log.borrow().len(), 10);
    assert!(runtime.start().is_err());
    assert!(runtime.add(Probe::new("c", &log)).is_err());
}

#[test]
fn queue_full_start_failure_cleans_up_every_initialized_plugin() {
    let log = Log::default();
    let mut runtime = Runtime::new(1);
    for name in ["a", "b"] {
        let mut plugin = Probe::new(name, &log);
        plugin.emit = true;
        runtime.add(plugin).unwrap();
    }
    assert!(runtime.start().unwrap_err().contains("queue is full"));
    assert_eq!(&log.borrow()[4..], &["b stop", "a stop"]);
    assert!(runtime.advance(Duration::ZERO).is_err());
}

#[test]
fn partial_initialize_is_stopped_but_unattempted_plugins_are_not() {
    let log = Log::default();
    let mut runtime = Runtime::new(8);
    let mut b = Probe::new("b", &log);
    b.fail_initialize = true;
    runtime.add(Probe::new("a", &log)).unwrap();
    runtime.add(b).unwrap();
    runtime.add(Probe::new("c", &log)).unwrap();
    assert!(runtime.start().unwrap_err().contains("init failed"));
    assert_eq!(&*log.borrow(), &["a init", "b init", "b stop", "a stop"]);
}

#[test]
fn stop_discards_queued_callbacks() {
    let log = Log::default();
    let mut runtime = Runtime::new(8);
    let mut a = Probe::new("a", &log);
    a.emit = true;
    runtime.add(a).unwrap();
    runtime.start().unwrap();
    runtime.stop().unwrap();
    assert_eq!(&*log.borrow(), &["a init", "a start", "a stop"]);
}

#[test]
fn handler_failure_stops_all_plugins_and_preserves_cleanup_failure() {
    let log = Log::default();
    let mut runtime = Runtime::new(8);
    let mut a = Probe::new("a", &log);
    a.emit = true;
    a.fail_message = true;
    let mut b = Probe::new("b", &log);
    b.fail_stop = true;
    runtime.add(a).unwrap();
    runtime.add(b).unwrap();
    runtime.start().unwrap();
    let error = runtime.advance(Duration::ZERO).unwrap_err();
    assert!(error.contains("handler failed"));
    assert!(error.contains("stop failed"));
    assert_eq!(&log.borrow()[5..], &["b stop", "a stop"]);
}

#[test]
fn continuous_messages_are_bounded_and_do_not_starve_ticks() {
    let log = Log::default();
    let mut runtime = Runtime::new(1);
    let mut flood = Probe::new("flood", &log);
    flood.emit = true;
    flood.flood = true;
    let mut timer_a = Probe::new("timer-a", &log);
    timer_a.interval = Some(Duration::from_secs(1));
    let ticks_a = timer_a.tick_count.clone();
    let mut timer_b = Probe::new("timer-b", &log);
    timer_b.interval = Some(Duration::from_secs(1));
    let ticks_b = timer_b.tick_count.clone();
    runtime.add(flood).unwrap();
    runtime.add(timer_a).unwrap();
    runtime.add(timer_b).unwrap();
    runtime.start().unwrap();
    runtime.advance(Duration::ZERO).unwrap();
    let before = log.borrow().len();
    runtime.advance(Duration::from_secs(1)).unwrap();
    assert_eq!(*ticks_a.borrow(), 1);
    assert_eq!(*ticks_b.borrow(), 1);
    assert_eq!(
        log.borrow().len() - before,
        3,
        "one broadcast reaches the publisher and two subscribers at capacity 1"
    );
    runtime.stop().unwrap();
}

#[test]
fn tick_failure_and_clock_overflow_stop_plugins() {
    let log = Log::default();
    let mut runtime = Runtime::new(8);
    let mut a = Probe::new("a", &log);
    a.interval = Some(Duration::from_secs(1));
    a.fail_tick = true;
    runtime.add(a).unwrap();
    assert!(runtime.start().is_ok());
    assert!(
        runtime
            .advance(Duration::from_secs(1))
            .unwrap_err()
            .contains("tick failed")
    );
    assert_eq!(&log.borrow()[2..], &["a stop"]);

    let mut runtime = Runtime::new(8);
    let mut timer = Probe::new("timer", &log);
    timer.interval = Some(Duration::from_secs(1));
    runtime.add(timer).unwrap();
    runtime.start().unwrap();
    runtime.advance(Duration::MAX).unwrap_err();
    assert!(runtime.advance(Duration::from_nanos(1)).is_err());
}

#[test]
fn duplicate_names_are_rejected() {
    let log = Log::default();
    let mut runtime = Runtime::new(8);
    runtime.add(Probe::new("a", &log)).unwrap();
    assert!(runtime.add(Probe::new("a", &log)).is_err());
}
