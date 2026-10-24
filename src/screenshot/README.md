# 截图服务

独立的单次截图模块，不依赖 OCR、推理库、Live2D、窗口渲染或 IPC 命令处理器。

```text
screenshot/
  IScreenshotService.hpp       对外接口、类型化错误和默认工厂
  PortalScreenshotService.*    Linux XDG Desktop Portal 实现
  CMakeLists.txt               deskwaifu_screenshot 构建目标
  tests/                      私有 D-Bus 上的模拟 Portal 测试
```

## 对外使用

调用者只需包含 `screenshot/IScreenshotService.hpp`，并链接 `deskwaifu_screenshot`。由上层协调者持有服务对象，生命周期必须覆盖异步请求：

```cpp
auto screenshots = CreateScreenshotService();
QObject::connect(screenshots.get(), &IScreenshotService::Captured, receiver,
    [](const QString& id, const QImage& image) {
        // 只处理这次请求的图像；是否送 OCR、保存或展示由上层决定。
    });
QObject::connect(screenshots.get(), &IScreenshotService::Failed, receiver,
    [](const QString& id, IScreenshotService::Error error, const QString& message) {
        // 可区分取消、不可用、后端失败、协议错误、图片读取失败和超时。
    });
const QString id = screenshots->Capture();
// 必要时：screenshots->Cancel(id);
```

- `Capture()` 返回不透明请求 ID；正在处理请求或清理解码任务时返回空字符串，不排队、不替换旧任务，也不额外发送 Failed。
- 已接受的请求在对象存活期间通过一次 `Captured` 或 `Failed` 结束。结果相对 Capture 调用异步返回；显式 Cancel 可以立即发出 Failed。
- `Cancel(id)` 仅取消匹配的活动请求，旧 ID 不会误取消新请求。已开始的图片解码无法强制中断，其结果会被丢弃；Busy 保持为真直到后台任务释放，避免取消后继续叠加解码任务。
- `Busy()` 包含等待 Portal、解码及取消后的解码清理。
- 销毁服务会关闭尚未完成的 Portal 请求，不再发送结果；后台解码不会借用已销毁的服务对象。晚到的 Portal 方法返回仍会尝试关闭对应句柄。
- `Capture(parentWindow)` 可传平台窗口标识，例如已导出的 `wayland:...`，不接受普通 Qt 指针或窗口整数代替；留空代表无父窗口。本模块不导出窗口句柄。

接口不暴露 QDBusConnection、Portal URI 或临时文件。返回图像坐标是图像像素，不承诺对应桌面逻辑坐标、屏幕原点或多屏/DPI 映射。截图范围与交互能力由当前 Portal 后端提供，本轮不提供指定矩形、连续帧或静默抓屏选项。

## Portal 实现

- 默认工厂创建 `PortalScreenshotService`，仅在 Capture 时调用 `org.freedesktop.portal.Screenshot`。构造对象不发起截图，不弹授权窗口。
- 始终传递 `interactive=true`；由系统 Portal 决定授权及截图 UI，不能承诺每个桌面后端都提供相同选区方式。
- 先订阅预测请求路径再异步调用 Screenshot，避免 Response 早于方法返回。返回路径不同时切换订阅。
- 完整请求（含图片解码）默认超时 60 秒；取消、超时及析构时尽力调用 Request.Close，不依赖关闭后仍收到 Response。
- 不做自动重试，也不回退到绕过 Portal 的抓屏方式。Portal 不可用时明确报告失败。
- 只读取无 host 的本地 `file:` URI，不接收 HTTP 等远程图片，也不删除/改写 Portal 提供的文件。
- 文件读取和解码使用 QtConcurrent，不阻塞渲染/事件循环线程。拒绝动画、无效图片、超过 64 MiB 的文件、单边超过 16384 像素或总像素超过 32 Mi 的图片；Qt 图片插件自身也可能施加额外分配限制。

应用运行时需系统提供 xdg-desktop-portal 和适合当前桌面的后端。构建使用 Qt6 Gui、DBus、Concurrent，均不引入 Python 或 OCR 依赖。

## 验证

```bash
cmake --build build-ocr-app --parallel 4
ctest --test-dir build-ocr-app -R '^screenshot-service$' --output-on-failure
```

CTest 使用 `dbus-run-session` 创建私有总线，并注册模拟 Portal，生成纯色图片验证结果；不调用用户当前桌面的 Portal，不采集真实截图。覆盖成功、响应提前、替代句柄、重复/陈旧响应、取消、超时、方法错误、无效 URI/图片、对象销毁和晚到句柄等场景。

本轮仅完成可复用模块，未加入截图 UI、桌宠外部截图命令，也未把截图自动串接到 OCR；真实桌面授权和不同 Portal 后端的人工验证尚未执行。
