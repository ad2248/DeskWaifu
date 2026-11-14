use deskwaifu_os::{Response, SOCKET_PATH, Server, call};
use deskwaifu_runtime::Runtime;
use deskwaifu_test_plugin::TestPlugin;
use std::{
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    thread,
    time::{Duration, Instant},
};

const USAGE: &str = "Usage:
  deskwaifu                       Start the platform
  deskwaifu COMMAND [ARGS...]      Send a command to the running platform
  deskwaifu test [ARGS...]         Run the test plugin command
  deskwaifu help                   Show registered command help
  deskwaifu --help                 Show this launcher help

Socket: /tmp/deskwaifu.socket";

fn run_host() -> Result<(), String> {
    let mut server = Server::bind(SOCKET_PATH)
        .map_err(|error| format!("cannot listen on {SOCKET_PATH}: {error}"))?;
    let running = Arc::new(AtomicBool::new(true));
    let signal = Arc::clone(&running);
    ctrlc::set_handler(move || signal.store(false, Ordering::Relaxed))
        .map_err(|error| format!("cannot register shutdown handler: {error}"))?;
    let mut runtime = Runtime::new(1024);
    runtime.add(TestPlugin::new())?;
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
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.is_empty() {
        return run_host();
    }
    if args.len() == 1 && matches!(args[0].as_str(), "--help" | "-h") {
        println!("{USAGE}");
        return Ok(());
    }
    // 子命令及其全部参数直接序列化；没有 serve 或 --socket 启动器参数。
    match call(SOCKET_PATH, args)
        .map_err(|error| format!("IPC {SOCKET_PATH}: {error}; start 'deskwaifu' first"))?
    {
        Response::Ok { output } => {
            println!("{output}");
            Ok(())
        }
        Response::Error { error } => Err(error),
    }
}
