# 系统控制插件

`system_gui_plugin` 提供控制面板中的系统操作页，负责 DeskWaifu 的启动、重启和进程状态控制。它不承载模型或桌面业务，只协调应用进程级操作。

## 实现结构

- `SystemGuiPlugin.cpp` 注册 GUI 插件并创建系统标签页；
- `SystemTab` 负责 Qt 页面和用户操作；
- `DeskWaifuLauncher` 封装新进程启动、参数和环境；
- `DeskWaifuRespawner` 封装重启/替换当前进程的流程。

控制页通过 launcher 发起新的 DeskWaifu 进程，通过 respawner 在当前进程退出前建立替代进程。实现将进程创建、重启和界面状态分开，避免把系统级操作散落到宿主入口或其他插件中；失败时保留错误状态并向界面报告，不把未成功启动伪装成正常退出。

## 与其他插件的关系

system 插件只依赖 GUI SDK 和系统进程能力。desktop、speech、IPC 等插件的资源释放由宿主生命周期负责；系统插件发起重启时应遵循宿主的停止顺序，确保动态插件和 GPU/音频等资源先完成清理。

## 验证

`tests/` 中的 `DeskWaifuLauncherTest` 和 `SystemRespawnerFixture` 覆盖启动参数、重启流程及测试夹具下的进程生命周期。构建控制插件时应一并运行这些测试。
