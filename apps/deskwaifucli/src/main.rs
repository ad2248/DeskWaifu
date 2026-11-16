//! 独立命令行客户端；不加载插件，也不启动主程序。
use deskwaifu_os::{Response, SOCKET_PATH, call};

const USAGE: &str = "Usage:
  deskwaifucli COMMAND [ARGS...]   Send a command to the running deskwaifu
  deskwaifucli test [ARGS...]      Run the test plugin command
  deskwaifucli help                Show registered command help
  deskwaifucli --help              Show this CLI help

Socket: /tmp/deskwaifu.socket
Start the main program separately: deskwaifu";

fn main() -> Result<(), String> {
    let args: Vec<String> = std::env::args().skip(1).collect();
    if args.is_empty() || (args.len() == 1 && matches!(args[0].as_str(), "--help" | "-h")) {
        println!("{USAGE}");
        return Ok(());
    }
    // 子命令及后续参数原样编码，不做 shell 拆分或启动器参数解析。
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
