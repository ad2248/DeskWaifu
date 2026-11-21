# DeskWaifu 控制面板（Wayland 原型）

面板在独立进程中通过 Unix socket 读取宿主的 `panel-tabs`，根据插件声明渲染标签页、文本、操作和可选的查询输入。操作沿用宿主的 `source=cli` 路由。对于返回操作编号且声明了完成查询的页面，面板会查询结果，然后用原本的输入值刷新动态区块。

## 启动

在 Linux Wayland 会话中使用近期 Rust 工具链。验证环境为 Rust 1.96.1、GPUI Kit 0.10.0、Sway 和 Mesa 软件 Vulkan。中文界面使用 Noto Sans CJK SC。构建需要常见 Linux 图形开发库，包括 Wayland、FreeType、XKB Common（含 X11 支持的构建依赖）和 XCB。

```sh
cargo build --manifest-path apps/terminal-panel/Cargo.toml --locked
cargo run --manifest-path apps/terminal-panel/Cargo.toml --locked
```

默认连接 `/tmp/deskwaifu.socket`。可以用 `DESKWAIFU_PANEL_SOCKET` 指定测试 socket。目录输入留空采用终端插件的默认目录。编辑目录时保留文本框的输入行为。在模型页且焦点不在目录编辑器时，Tab 和 Shift+Tab 分别切换到下一个和上一个模型。关闭窗口会退出面板进程。

插件查询页可以返回 `bindings: [{"key":"ctrl-alt-k","args":["plugin","command"]}]`。面板连接后在后台用默认参数查询其他标签页，所以不必先打开快捷键所属页。绑定使用小写 GPUI 键名，支持字母、数字、`f1`–`f24`、`space`、`enter`、`escape`、`backspace`、`delete`、`insert`、`home`、`end`、`pageup`、`pagedown` 和方向键。修饰键顺序为 `ctrl-alt-shift-super`，仅写实际需要的修饰键。`tab` 保留给现有切换操作。快捷键在面板有键盘焦点且不在编辑时响应，每次按下只执行一次。参数必须以声明绑定的插件名及其命令开头。多个不同命令声明同一组合键时，该组合键停用，按钮仍可用。

插件可以声明一组编辑字段和开始、预览、应用、取消命令。应用会先把表单当前值发送给插件，再提交事务。编辑期间面板使用同一条 IPC 连接，并定期查询状态以保活。关闭面板会关闭事务连接，宿主可据此撤销未提交的预览。

如需并入根 workspace，在根 manifest 的 `workspace.members` 添加 `apps/terminal-panel`，去掉原型 manifest 的 `[workspace]`，并重新生成根锁文件。独立锁文件只用于当前原型。
