use super::*;
use deskwaifu_sdk::{
    AudioStream, ClipboardWriteError, ConnectionId, Context, LocalFuture, Message, OsError,
    OsServices, Plugin, PluginResult,
};
use std::{
    cell::{Cell, RefCell},
    rc::Rc,
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    },
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
    fn plugin_id(&self) -> &str {
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

struct GatedClipboard {
    release: Arc<tokio::sync::Notify>,
    owner: ThreadId,
}

impl OsServices for GatedClipboard {
    fn capture_audio(&self, _: Duration) -> LocalFuture<'_, Result<AudioStream, OsError>> {
        unreachable!()
    }

    fn read_clipboard_text(&self) -> LocalFuture<'_, Result<Option<String>, OsError>> {
        Box::pin(async {
            assert_eq!(thread::current().id(), self.owner);
            assert_eq!(
                tokio::runtime::Handle::current().runtime_flavor(),
                tokio::runtime::RuntimeFlavor::CurrentThread
            );
            self.release.notified().await;
            Ok(Some("copied text".into()))
        })
    }

    fn write_clipboard_text(&self, _: String) -> LocalFuture<'_, Result<(), ClipboardWriteError>> {
        unreachable!()
    }
}

struct ClipboardProbe {
    phase: Rc<Cell<&'static str>>,
    complete: Arc<tokio::sync::Notify>,
}

impl Plugin for ClipboardProbe {
    fn plugin_id(&self) -> &str {
        "clipboard-probe"
    }

    fn initialize(&mut self, cx: &mut Context<'_>) -> PluginResult {
        cx.register_command("read", vec![])?;
        cx.register_command("phase", vec![])
    }

    fn on_command(
        &mut self,
        name: &str,
        _: &[String],
        cx: &mut Context<'_>,
    ) -> Result<String, String> {
        if name == "phase" {
            return Ok(self.phase.get().into());
        }
        if self.phase.get() == "pending" {
            return Err("busy".into());
        }
        self.phase.set("pending");
        let phase = self.phase.clone();
        let complete = self.complete.clone();
        let os = cx.os();
        cx.spawn_local(async move {
            assert_eq!(
                os.read_clipboard_text().await.unwrap().as_deref(),
                Some("copied text")
            );
            phase.set("done");
            complete.notify_one();
        })?;
        Ok("accepted".into())
    }

    fn on_message(&mut self, _: &Message, _: &mut Context<'_>) -> PluginResult {
        Ok(())
    }
}

// 测试目的：确认插件的 OS 操作由宿主当前线程驱动，等待期间 IPC 仍能查询状态和拒绝重复请求。
// 测试方法：用 Notify 挂起剪贴板替身，从真实 Unix socket 查询状态并再次触发，释放任务后检查完成状态。
#[tokio::test(flavor = "current_thread")]
async fn commands_remain_responsive_while_local_os_future_is_pending() {
    use crate::{Request, Response};
    use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
    use tokio::net::UnixStream;
    use tokio::sync::Notify;

    async fn call(peer: &mut BufReader<UnixStream>, name: &str) -> Response {
        let mut request = serde_json::to_vec(&Request::cli(vec![name.into()])).unwrap();
        request.push(b'\n');
        peer.write_all(&request).await.unwrap();
        let mut response = String::new();
        peer.read_line(&mut response).await.unwrap();
        serde_json::from_str(&response).unwrap()
    }

    let release = Arc::new(Notify::new());
    let complete = Arc::new(Notify::new());
    let shutdown = Notify::new();
    let mut runtime = Runtime::new(16);
    runtime
        .set_os_services(Rc::new(GatedClipboard {
            release: release.clone(),
            owner: thread::current().id(),
        }))
        .unwrap();
    runtime
        .add(ClipboardProbe {
            phase: Rc::new(Cell::new("idle")),
            complete: complete.clone(),
        })
        .unwrap();
    runtime.start().unwrap();
    let mut server = server();
    let path = server.path.clone();
    let client = async {
        let mut peer = BufReader::new(UnixStream::connect(path).await.unwrap());
        assert!(
            matches!(call(&mut peer, "read").await, Response::Ok { output } if output == "accepted")
        );
        assert!(
            matches!(call(&mut peer, "phase").await, Response::Ok { output } if output == "pending")
        );
        assert!(
            matches!(call(&mut peer, "read").await, Response::Error { error } if error == "busy")
        );
        release.notify_one();
        complete.notified().await;
        assert!(
            matches!(call(&mut peer, "phase").await, Response::Ok { output } if output == "done")
        );
        shutdown.notify_one();
    };
    let (result, ()) = tokio::time::timeout(Duration::from_secs(5), async {
        tokio::join!(
            run_loop(&mut runtime, &mut server, shutdown.notified()),
            client
        )
    })
    .await
    .unwrap();
    result.unwrap();
    runtime.stop().unwrap();
}

struct ConnectionOwner {
    owner: Option<ConnectionId>,
    disconnected: Rc<RefCell<Vec<ConnectionId>>>,
    released: Rc<tokio::sync::Notify>,
    fail_tick: Rc<Cell<bool>>,
}

impl Plugin for ConnectionOwner {
    fn plugin_id(&self) -> &str {
        "connection-owner"
    }

    fn initialize(&mut self, cx: &mut Context<'_>) -> PluginResult {
        assert_eq!(cx.connection_id(), None);
        cx.register_command("claim", vec![])
    }

    fn on_command(
        &mut self,
        _: &str,
        _: &[String],
        cx: &mut Context<'_>,
    ) -> Result<String, String> {
        let id = cx.connection_id().ok_or("connection required")?;
        if self.owner.is_some_and(|owner| owner != id) {
            return Err("owned by another connection".into());
        }
        self.owner = Some(id);
        Ok(id.0.to_string())
    }

    fn on_disconnect(&mut self, id: ConnectionId, cx: &mut Context<'_>) {
        assert_eq!(cx.connection_id(), None);
        if self.owner == Some(id) {
            self.owner = None;
            self.released.notify_one();
        }
        self.disconnected.borrow_mut().push(id);
    }

    fn on_message(&mut self, _: &Message, _: &mut Context<'_>) -> PluginResult {
        Ok(())
    }

    fn tick_interval(&self) -> Option<Duration> {
        Some(Duration::from_millis(5))
    }

    fn tick(&mut self, _: &mut Context<'_>) -> PluginResult {
        if self.fail_tick.get() {
            return Err("tick failed".into());
        }
        Ok(())
    }

    fn stop(&mut self, _: &mut Context<'_>) -> PluginResult {
        assert_eq!(self.owner, None, "disconnect must run before stop");
        Ok(())
    }
}

// 通过两个真实 socket 验证持久连接的身份、EOF 后的所有权交接和退出时的清理。
#[tokio::test]
async fn connection_owner_is_released_on_eof_and_host_exit() {
    use crate::Response;
    use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
    use tokio::net::UnixStream;
    use tokio::sync::Notify;

    async fn claim(peer: &mut BufReader<UnixStream>) -> Response {
        peer.write_all(b"{\"source\":\"cli\",\"payload\":[\"claim\"]}\n")
            .await
            .unwrap();
        let mut response = String::new();
        peer.read_line(&mut response).await.unwrap();
        serde_json::from_str(&response).unwrap()
    }

    for fatal in [false, true] {
        let shutdown = Notify::new();
        let released = Rc::new(Notify::new());
        let disconnected = Rc::new(RefCell::new(Vec::new()));
        let fail_tick = Rc::new(Cell::new(false));
        let mut runtime = Runtime::new(16);
        runtime
            .add(ConnectionOwner {
                owner: None,
                disconnected: disconnected.clone(),
                released: released.clone(),
                fail_tick: fail_tick.clone(),
            })
            .unwrap();
        runtime.start().unwrap();
        assert_eq!(
            runtime.dispatch_command(&["claim".into()]),
            Err("connection required".into())
        );
        let mut server = server();
        let path = server.path.clone();
        let client = async {
            let mut first = BufReader::new(UnixStream::connect(&path).await.unwrap());
            let mut second = BufReader::new(UnixStream::connect(&path).await.unwrap());
            let Response::Ok { output: first_id } = claim(&mut first).await else {
                panic!("first connection must claim ownership");
            };
            assert_eq!(
                claim(&mut first).await,
                Response::Ok {
                    output: first_id.clone()
                }
            );
            assert_eq!(
                claim(&mut second).await,
                Response::Error {
                    error: "owned by another connection".into()
                }
            );
            drop(first);
            released.notified().await;
            let Response::Ok { output: second_id } = claim(&mut second).await else {
                panic!("second connection must claim ownership after EOF");
            };
            assert_ne!(first_id, second_id);
            if fatal {
                fail_tick.set(true);
            } else {
                shutdown.notify_one();
            }
            // 保持第二个客户端存活，直到宿主主动关闭该连接。
            released.notified().await;
            [first_id, second_id].map(|id| ConnectionId(id.parse().unwrap()))
        };
        let (result, ids) = tokio::time::timeout(Duration::from_secs(2), async {
            tokio::join!(
                run_loop(&mut runtime, &mut server, shutdown.notified()),
                client
            )
        })
        .await
        .unwrap();
        assert_eq!(*disconnected.borrow(), ids);
        assert!(server.clients.is_empty());
        if fatal {
            assert_eq!(result.unwrap_err(), "connection-owner tick: tick failed");
        } else {
            result.unwrap();
        }
        runtime.stop().unwrap();
    }
}
