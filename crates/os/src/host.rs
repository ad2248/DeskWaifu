//! 平台宿主：监听 socket、驱动插件运行时，并在退出时清理资源。
use crate::{SOCKET_PATH, Server};
use deskwaifu_runtime::Runtime;
use std::future::Future;
use tokio::{
    signal::unix::{SignalKind, signal},
    time::Instant,
};

async fn run_loop(
    runtime: &mut Runtime,
    server: &mut Server,
    shutdown: impl Future<Output = ()>,
) -> Result<(), String> {
    let mut previous = Instant::now();
    tokio::pin!(shutdown);
    loop {
        let now = Instant::now();
        runtime.advance(now.duration_since(previous))?;
        previous = now;
        let wakeup = runtime.next_wakeup();
        let timer = async {
            match wakeup {
                Some(delay) => tokio::time::sleep_until(previous + delay).await,
                None => std::future::pending::<()>().await,
            }
        };
        tokio::select! {
            biased;
            _ = &mut shutdown => return Ok(()),
            result = server.poll_ready(|argv| runtime.dispatch_command(argv)) => {
                result.map_err(|error| format!("IPC listener: {error}"))?;
            }
            _ = timer => {},
        }
        // 即使消息持续入队或连接持续就绪，也让同线程的其他异步任务得到执行机会。
        tokio::task::yield_now().await;
    }
}

/// 在调用者的异步上下文中运行平台宿主，直到收到 SIGINT / SIGTERM。
/// 调用者应使用主线程上的 Tokio current_thread 运行时，并提前加载插件。
/// 本函数负责绑定 IPC、启动插件、驱动调度和退出清理，不创建后台任务。
pub async fn run(mut runtime: Runtime) -> Result<(), String> {
    let mut server = Server::bind(SOCKET_PATH)
        .map_err(|error| format!("cannot listen on {SOCKET_PATH}: {error}"))?;
    // 启动插件前安装信号监听，保留 SIGINT / SIGTERM 的优雅退出行为。
    let mut interrupt = signal(SignalKind::interrupt())
        .map_err(|error| format!("cannot register shutdown handler: {error}"))?;
    let mut terminate = signal(SignalKind::terminate())
        .map_err(|error| format!("cannot register shutdown handler: {error}"))?;
    let shutdown = async move {
        tokio::select! {
            _ = interrupt.recv() => {},
            _ = terminate.recv() => {},
        }
    };
    runtime.start()?;
    eprintln!("Listening on {SOCKET_PATH}");
    let result = run_loop(&mut runtime, &mut server, shutdown).await;
    let cleanup = runtime.stop();
    result.and(cleanup)
}

#[cfg(test)]
mod tests;
