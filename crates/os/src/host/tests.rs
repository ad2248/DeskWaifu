use super::*;
use deskwaifu_sdk::{Context, Message, Plugin, PluginResult};
use std::{
    cell::RefCell,
    rc::Rc,
    sync::atomic::{AtomicUsize, Ordering},
    thread::{self, ThreadId},
    time::Duration,
};

struct Probe {
    thread: ThreadId,
    events: Rc<RefCell<Vec<&'static str>>>,
    fail_tick: bool,
}

impl Probe {
    fn record(&self, event: &'static str) {
        assert_eq!(thread::current().id(), self.thread);
        assert_eq!(
            tokio::runtime::Handle::current().runtime_flavor(),
            tokio::runtime::RuntimeFlavor::CurrentThread
        );
        self.events.borrow_mut().push(event);
    }
}

impl Plugin for Probe {
    fn name(&self) -> &str {
        "probe"
    }

    fn initialize(&mut self, _: &mut Context<'_>) -> PluginResult {
        self.record("initialize");
        Ok(())
    }

    fn start(&mut self, _: &mut Context<'_>) -> PluginResult {
        self.record("start");
        Ok(())
    }

    fn tick_interval(&self) -> Option<Duration> {
        Some(Duration::from_millis(5))
    }

    fn tick(&mut self, _: &mut Context<'_>) -> PluginResult {
        self.record("tick");
        if self.fail_tick {
            return Err("tick failed".into());
        }
        Ok(())
    }

    fn on_message(&mut self, _: &Message, _: &mut Context<'_>) -> PluginResult {
        Ok(())
    }

    fn stop(&mut self, _: &mut Context<'_>) -> PluginResult {
        self.record("stop");
        Ok(())
    }
}

fn server() -> Server {
    static NEXT: AtomicUsize = AtomicUsize::new(0);
    let path = std::env::temp_dir().join(format!(
        "deskwaifu-async-{}-{}.socket",
        std::process::id(),
        NEXT.fetch_add(1, Ordering::Relaxed)
    ));
    Server::bind(path).unwrap()
}

fn runtime(fail_tick: bool) -> (Runtime, Rc<RefCell<Vec<&'static str>>>) {
    let events = Rc::new(RefCell::new(Vec::new()));
    let mut runtime = Runtime::new(16);
    runtime
        .add(Probe {
            thread: thread::current().id(),
            events: Rc::clone(&events),
            fail_tick,
        })
        .unwrap();
    runtime.start().unwrap();
    (runtime, events)
}

#[tokio::test(flavor = "current_thread", start_paused = true)]
async fn idle_host_waits_without_periodic_polling() {
    let mut runtime = Runtime::new(16);
    runtime.start().unwrap();
    let mut server = server();
    let polls = std::cell::Cell::new(0);
    let sleep = tokio::time::sleep(Duration::from_secs(60));
    tokio::pin!(sleep);
    let shutdown = std::future::poll_fn(|cx| {
        polls.set(polls.get() + 1);
        sleep.as_mut().poll(cx)
    });
    run_loop(&mut runtime, &mut server, shutdown).await.unwrap();
    // 固定 5 ms 轮询会使 shutdown 被反复 poll；空闲监听应一直挂起到退出。
    assert!(
        polls.get() <= 3,
        "idle host was polled {} times",
        polls.get()
    );
    runtime.stop().unwrap();
}

#[tokio::test(flavor = "current_thread", start_paused = true)]
async fn async_loop_keeps_non_send_plugins_on_the_calling_thread() {
    let (mut runtime, events) = runtime(false);
    let mut server = server();
    // 虚拟时间自动推进：若主循环阻塞休眠而不让出执行器，此测试无法完成。
    run_loop(
        &mut runtime,
        &mut server,
        tokio::time::sleep(Duration::from_millis(16)),
    )
    .await
    .unwrap();
    runtime.stop().unwrap();
    assert_eq!(
        *events.borrow(),
        ["initialize", "start", "tick", "tick", "tick", "stop"]
    );
}

#[tokio::test(flavor = "current_thread", start_paused = true)]
async fn async_loop_propagates_tick_errors_and_stops_plugins_once() {
    let (mut runtime, events) = runtime(true);
    let mut server = server();
    let error = run_loop(&mut runtime, &mut server, std::future::pending())
        .await
        .unwrap_err();
    assert_eq!(error, "probe tick: tick failed");
    runtime.stop().unwrap();
    assert_eq!(*events.borrow(), ["initialize", "start", "tick", "stop"]);
}
