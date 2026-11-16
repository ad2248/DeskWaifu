//! 仓库内部 Rust 动态插件：不承诺稳定 ABI。
use super::{Runtime, State};
use deskwaifu_sdk::{PLUGIN_ENTRY_SYMBOL, PluginCreate, PluginId};
use libloading::Library;
use std::{fs, path::Path};

impl Runtime {
    /// 加载一个插件，保留动态库直至整个 runtime 销毁。
    ///
    /// # Safety
    /// 路径必须指向可信、与宿主使用相同 Rust 工具链及同版 SDK/依赖构建的库。
    /// 导出入口必须是 `fn() -> Box<dyn Plugin>`，名为 `deskwaifu_plugin_create`。
    /// 不检测或承诺 Rust ABI 兼容性；不兼容库可能导致未定义行为。
    /// 插件必须在 stop 中结束自己的工作线程；动态库产生的值、消息负载和函数
    /// 不得被保留到 runtime 销毁之后，否则其析构函数或回调地址可能已经卸载。
    pub unsafe fn load_plugin(&mut self, path: impl AsRef<Path>) -> Result<PluginId, String> {
        if self.state != State::Created {
            return Err("plugins must be loaded before start".into());
        }
        let path = path.as_ref();
        // SAFETY: 调用者保证库及其初始化代码可信且 ABI 兼容。
        let library = unsafe { Library::new(path) }
            .map_err(|error| format!("load plugin {}: {error}", path.display()))?;
        // SAFETY: 调用者保证此符号具有 PluginCreate 的 Rust 签名。
        let create = unsafe { library.get::<PluginCreate>(PLUGIN_ENTRY_SYMBOL) }
            .map_err(|error| format!("plugin entry {}: {error}", path.display()))?;
        let plugin = create();
        // 失败时 plugin 在 library 之前销毁；成功后 runtime 字段顺序保证相同次序。
        let id = self.add_boxed(plugin)?;
        self.libraries.push(library);
        Ok(id)
    }

    /// 按文件名排序加载目录中的所有 `.so`；忽略子目录和非 .so 文件。
    /// 任一库加载失败会停止 runtime，不能启动残缺的插件集合。
    ///
    /// # Safety
    /// 目录及目录内的所有 `.so` 必须满足 `load_plugin` 的安全约束。
    /// 动态加载会执行库代码；此目录不应允许不可信用户写入。
    pub unsafe fn load_plugins_from(
        &mut self,
        directory: impl AsRef<Path>,
    ) -> Result<usize, String> {
        if self.state != State::Created {
            return Err("plugins must be loaded before start".into());
        }
        let result = (|| {
            let directory = directory.as_ref();
            let mut paths = Vec::new();
            for entry in fs::read_dir(directory)
                .map_err(|error| format!("plugin directory {}: {error}", directory.display()))?
            {
                let path = entry.map_err(|error| error.to_string())?.path();
                if path.extension().is_some_and(|extension| extension == "so") && path.is_file() {
                    paths.push(path);
                }
            }
            paths.sort();
            for path in &paths {
                // SAFETY: 调用者对整个目录保证相同的可信性和兼容性约束。
                unsafe { self.load_plugin(path)? };
            }
            Ok(paths.len())
        })();
        if result.is_err() {
            // 加载阶段尚未 initialize；使 runtime 失效，Drop 仍会先销毁实例再卸载库。
            let _ = self.stop();
        }
        result
    }
}
