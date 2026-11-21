use deskwaifu_runtime::Runtime;

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

// 不启用工作线程池：平台主循环与所有插件回调始终在进程主线程执行。
#[tokio::main(flavor = "current_thread")]
async fn main() -> Result<(), String> {
    if std::env::args_os().nth(1).is_some() {
        return Err("deskwaifu does not accept arguments; use deskwaifucli for commands".into());
    }
    let mut runtime = Runtime::new(1024);
    let absolute_env = |name| {
        std::env::var_os(name)
            .map(std::path::PathBuf::from)
            .filter(|path| path.is_absolute())
    };
    if let Some(root) = absolute_env("XDG_CONFIG_HOME")
        .or_else(|| absolute_env("HOME").map(|home| home.join(".config")))
    {
        runtime.load_config(root.join("deskwaifu/config.jsonc"))?;
    }
    load_plugins(&mut runtime)?;
    deskwaifu_os::run(runtime).await
}
