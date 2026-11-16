use deskwaifu_os::{SOCKET_PATH, Server};
use deskwaifu_runtime::Runtime;
use std::{
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    thread,
    time::{Duration, Instant},
};

// 仅加载可执行文件旁由同一仓库构建、由用户部署的可信插件。
#[allow(unsafe_code)]
fn load_plugins(runtime: &mut Runtime) -> Result<(), String> {
    let executable = std::env::current_exe().map_err(|error| error.to_string())?;
    let directory = executable
        .parent()
        .ok_or("cannot locate executable directory")?
        .join("plugins");
    // SAFETY: 本项目不承诺跨版本 ABI。部署约定要求插件与宿主同工具链同代码构建，
    // 安装目录只包含可信插件；不能加载下载的任意 .so。
    unsafe { runtime.load_plugins_from(&directory)? };
    Ok(())
}

fn run_host() -> Result<(), String> {
    let mut server = Server::bind(SOCKET_PATH)
        .map_err(|error| format!("cannot listen on {SOCKET_PATH}: {error}"))?;
    let running = Arc::new(AtomicBool::new(true));
    let signal = Arc::clone(&running);
    ctrlc::set_handler(move || signal.store(false, Ordering::Relaxed))
        .map_err(|error| format!("cannot register shutdown handler: {error}"))?;
    let mut runtime = Runtime::new(1024);
    load_plugins(&mut runtime)?;
    runtime.start()?;
    eprintln!("Listening on {SOCKET_PATH}");
    let mut previous = Instant::now();
    let result = (|| {
        while running.load(Ordering::Relaxed) {
            let now = Instant::now();
            runtime.advance(now.duration_since(previous))?;
            previous = now;
            server
                .poll(|argv| runtime.dispatch_command(argv))
                .map_err(|error| format!("IPC listener: {error}"))?;
            thread::sleep(Duration::from_millis(5));
        }
        Ok(())
    })();
    let cleanup = runtime.stop();
    result.and(cleanup)
}

fn main() -> Result<(), String> {
    if std::env::args_os().nth(1).is_some() {
        return Err("deskwaifu does not accept arguments; use deskwaifucli for commands".into());
    }
    run_host()
}
