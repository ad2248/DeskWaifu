# Qt 模型窗口

> 核对基线：`0058b7e`。窗口与线程边界是当前约束；定时器、FBO 和测试实现是现行设计。窗口后端迁移背景见[实施记录](../实施日志/插件与窗口迁移.md)，本次未运行图形测试。

Qt 是唯一模型窗口实现，控制面板仍是独立进程。Cubism 模型/会话、应用状态、编辑与配置属于 desktop，窗口端口不承担这些业务；JSON IPC 属于独立 IPC 插件。

## 构建与开发运行

依赖 Qt **6.5+**（Gui、OpenGL、WaylandClient、StateMachine、Concurrent、DBus）、Qt Wayland 平台插件、LayerShellQt **6.6+**、桌面 OpenGL **3.3 compatibility profile**、EGL、libsystemd 开发文件、Cubism 和 GLEW。

默认控制面板另需 Widgets/Network；默认语音插件另需 WebSockets/Multimedia，关闭 `DESKWAIFU_BUILD_SPEECH_PLUGIN` 可省后两者，但不满足完整门禁。默认 `BUILD_TESTING=ON` 的完整测试还需 Qt Network/Test、wayland-scanner 与 wayland-client 开发文件。Arch 的 LayerShellQt 包名为 `layer-shell-qt`，其他发行版可能拆分开发包。默认原生 OCR helper 的额外依赖见[构建依赖](构建依赖.md)。

后端和 GUI 默认从安装前缀发现插件，不自动指向构建树。开发时显式设置目录；以下命令从仓库根运行，模型路径和外部依赖需替换为本机真实路径：

```sh
cmake -S . -B build-qt -DDESKWAIFU_BUILD_OCR=OFF
cmake --build build-qt
DESKWAIFU_PLUGIN_DIR="$PWD/build-qt/plugins" \
  QT_QPA_PLATFORM=wayland build-qt/bin/deskwaifu --models_dir /path/to/models
DESKWAIFU_CONTROL_PLUGIN_DIR="$PWD/build-qt/plugins" build-qt/bin/deskwaifu-control
```

此例显式省略 OCR helper，不代表默认源码构建关闭它。独立工作树可通过 `SDK_ROOT_PATH`、`GLEW_PATH` 使用源码外依赖。关闭 CONTROL 仅省控制面板，不能省 Qt 模型窗口。运行配置详见[插件架构](插件架构.md)和[GUI 插件](GUI插件.md)。不要让多个模型进程争用相同 runtime socket。

## 需求与窗口边界

- **WIN-01**：desktop 在非 Wayland 或无 layer-shell 的合成器上拒绝初始化，不能回退普通桌面窗口。其他健康插件和平台可继续运行，不应把进程存活当作窗口可用。
- **WIN-02**：`QtDesktopWindow` 用 QWindow、QOpenGLContext 和 LayerShellQt 实现窗口/输出/输入端口。Qt 管理自己的连接、surface、configure 确认及 EGL 集成；应用不自行 dispatch Qt 的 Wayland socket 或管理其 EGL 句柄。
- **WIN-03**：等待窗口 exposed 后绑定呈现 context。所需 compatibility profile 支持当前 Cubism/sprite 的客户端顶点/索引数组；拒绝 core-only 或 OpenGL ES。Qt 6.5+ 通过 `LayerShellQt::Window::get()` 按窗口选择 shell 集成，不要求全局 `Shell::useLayerShell()`。
- **WIN-04**：GUI 线程呈现窗口，专用 model-render 线程在独立 offscreen context/FBO 处理模型；跨线程使用不可变 CPU 帧，不借用 Cubism/GL 对象。默认目标为 QOffscreenSurface/QOpenGLContext，显式 PCI 设备用 EGL context，但不是另一套原生窗口后端。
- **WIN-05**：裁剪、输入轮廓和鼠标坐标统一为输出局部逻辑像素；Qt 只缩放最终呈现到实际 buffer。当前按逻辑分辨率绘制，不承诺 HiDPI 模型超采样或零拷贝。
- **WIN-06**：输入轮廓从模型 alpha 产生，早于透明度乘数和编辑装饰；空轮廓显式使用 `Qt::WindowTransparentForInput`，不能依赖空 setMask（其语义是恢复全窗口输入）。普通轮廓恢复输入，屏幕编辑接受整输出输入。
- **WIN-07**：双击事件作为按下交给统一点击策略；Wayland XKB 扫描码转换回 evdev。忽略自动重复，失焦释放 layer keyboard interactivity 并清除按键。不新增全局快捷键；不能因 Qt 尚未枚举键盘而拒绝双击焦点请求。
- **WIN-08**：QScreen 存活期间输出 ID 稳定，移除不重排其他 ID，重连分配新 ID。切屏复用呈现 context，配置期间抑制输入/嵌套呈现；失败尽可能回滚旧屏，当前屏消失时回退其他屏，无屏或重绑定失败请求关闭。
- **WIN-09**：先停止并收尾模型线程，再释放呈现 FBO/context/window。恢复 context 失败时不得无上下文删除 GL 对象，失败向上传递；细则见[GPU 生命周期](GPU生命周期.md)。

## 现行调度与限制

GUI 定时器通常每 16 ms 呈现，输入区域为空时降为 33 ms；模型线程仍每 16 ms 绘制，空输入不意味着模型渲染降频。配置轮询由呈现驱动。IPC 以 QSocketNotifier 处理就绪事件，输出替换期间暂停，不由逐帧轮询决定命令进展。

RGBA 从模型 context 读回 CPU 再上传窗口 FBO；不是共享纹理。窗口只转发 `IDesktopWindowEvents`，不暴露原生协议对象。Qt 不授予 Wayland 全局指针权限，KWin 注视与 Hyprland/Sway 焦点/跨屏能力见[Wayland 集成](Wayland集成.md)；实体鼠标和画面质量仍需真实桌面检查。

## 验收入口与证据边界

```sh
ctest --test-dir build-qt --output-on-failure
python3 tests/qt_headless_smoke.py build-qt /path/to/CubismSdkForNative-5-r.5
python3 tests/qt_headless_smoke.py build-qt /path/to/CubismSdkForNative-5-r.5 --scale 1.5
```

这些是可执行入口，不是本次通过记录。smoke 要求 BUILD_TESTING、Python、支持虚拟输出/fake-input v3 的 kwin_wayland、dbus-run-session 和 SDK Haru 模型；它创建私有 D-Bus、双虚拟输出、runtime/config 并使用软件渲染。非 1 倍缩放必须有 kscreen-doctor；存在该命令时还执行输出禁用/重启。输入仅注入私有合成器，不能连接用户桌面。日志在 `BUILD_DIR/qt-smoke-logs/<scale>/run-*/`。

窗口集成测试检查模型 FBO 和 swap 前真实窗口 back buffer 的颜色/alpha，呈现前故意设 GL_NONE，覆盖 NVIDIA 不可见窗口回归、切屏和小数缩放。另检查逻辑 mask、空输入恢复、合成 Qt 鼠标/键盘、焦点与 teardown。smoke 还检查跨 context 帧传输、真实 compositor 点击路由及 Wayland 输入区域请求，并执行真实模型动作/表情/口型/视线、裁剪保存、屏幕编辑、显隐、模型切换和 quit。

Sway smoke 特意无输入设备，通过 `--no-keyboard` 与协议 trace 检查无键盘 seat；KWin 场景则要求可用键盘和真实窗口焦点。两者不能互相替代。Layer-shell trace 必须有 `get_layer_surface`，不能出现 `get_xdg_surface` 回退。

已知 Cubism shader set 共享 program ID，退出时可能出现 `GL_INVALID_VALUE in glDeleteProgram`；只允许 quit 之后这一精确诊断，渲染期间的 GL 错误仍失败。不修改第三方 SDK 掩盖错误。

`DESKWAIFU_QT_LIVE_TEST=ON` 将完整窗口集成注册到 CTest，只能在隔离 compositor 使用。私有软件环境不能替代实体鼠标穿透、桌面焦点策略、真实 GPU 驱动或视觉质量验收。
