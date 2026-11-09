# DeskWaifu 插件总览

`plugins/` 是 DeskWaifu 的可选功能集合。插件通过公共 Plugin SDK 编译为动态模块，在宿主启动时按构建配置加载；插件之间不通过直接持有对象协作，而是通过 `PluginContext` 创建的事件端点、能力（capability）和请求/响应消息通信。

## 分层与关系

```text
外部控制 / 控制面板
        │
        ├── ipc ──> command.* /v1 ──> desktop、perception、reading_clip、speech
        │                                      │
        │                                      └── interaction.*、speech.* 等领域事件
        │
        └── system、interaction、model（GUI 控制插件）

桌面运行时：desktop ──> 模型、窗口、输入、渲染
感知能力：perception ──> 截图、剪贴板图像、OCR
内容处理：reading_clip ──> 朗读文本与状态
语音能力：speech ──> 云端语音源、PCM 播放
```

- **`desktop`** 是核心运行时插件，拥有模型、渲染窗口、输入和桌面交互；它发布模型与交互命令，并发布双击等领域事件。它不直接依赖 IPC、语音、朗读或 OCR。
- **`ipc`** 是外部命令入口，将 Unix socket 等传输转换为插件命令请求，再由命令路由分发到提供对应 `command.<name>/v1` 能力的插件。
- **`perception`** 提供截图、图像处理和 OCR 命令；OCR 后端通过独立客户端/模块加载，避免把具体 OCR 实现耦合进桌面运行时。
- **`reading_clip`** 编排“读取图像 → OCR 识别 → 语音播报”的朗读流水线，并发布朗读状态；图像、OCR 和语音能力分别由对应插件提供。
- **`speech`** 提供 `speech.speak/v1` 和语音设置命令，并订阅 `interaction.double_click/v1`。双击问候、请求取消、云端合成和音频播放都在该插件内部完成。
- **`system`、`interaction`、`model`** 是控制面板 GUI 插件。它们通过 GUI SDK 创建标签页或控制面板，不拥有对应核心业务；具体命令仍由运行时插件和事件端点处理。

## 构建与加载

顶层 `plugins/CMakeLists.txt` 根据 `DESKWAIFU_BUILD_*` 选项加入插件子目录。`deskwaifu_add_plugin()` 会生成不带 `lib` 前缀的动态模块，并输出到 `build/plugins/<name>/`，安装到 `deskwaifu/plugins/<name>/`。GUI 插件额外链接 `deskwaifu_gui_plugin_sdk` 并启用 Qt 自动生成。

每个动态插件都导出 SDK v1 入口，生命周期通常为：

1. `Initialize`：创建端点、读取配置、注册能力和订阅关系；
2. `Start`：创建处理器、线程或外部传输并开始工作；
3. 事件循环中处理请求、通知和取消；
4. `Stop`：先停止输入源和异步处理，再释放处理器与端点。

## 通信约定

命令能力采用 `command.<name>/v1`，请求通常携带 JSON，响应通过 `command.result` 返回。领域能力采用带版本的主题，例如 `speech.speak/v1`。插件应校验请求主题与正文中的命令一致，并在停止时清理未完成请求，避免迟到事件访问已销毁对象。

## 插件目录

| 插件 | 类型 | 主要职责 |
| --- | --- | --- |
| [desktop](desktop/README.md) | 运行时 | 模型、渲染、窗口、输入与桌面命令 |
| [perception](perception/README.md) | 运行时 | 截图、图像处理与 OCR |
| [ipc](ipc/README.md) | 运行时 | IPC 传输与外部命令网关 |
| [reading_clip](reading_clip/README.md) | 运行时 | 朗读文本处理与状态管理 |
| [speech](speech/README.md) | 运行时 | 语音合成、音频播放与语音配置 |
| [system](system/README.md) | GUI | 启动、重启和系统控制 |
| [interaction](interaction/README.md) | GUI | 交互设置与快捷键面板 |
| [model](model/README.md) | GUI | 模型控制面板 |

## 测试入口

插件测试源码位于各插件的 `tests/` 目录，测试目标只在对应构建选项开启时注册：`DESKWAIFU_BUILD_DESKTOP_PLUGIN`、`DESKWAIFU_BUILD_PERCEPTION_PLUGIN`、`DESKWAIFU_BUILD_IPC_PLUGIN`、`DESKWAIFU_BUILD_READING_CLIP_PLUGIN`、`DESKWAIFU_BUILD_SPEECH_PLUGIN` 分别控制运行时插件，`system`、`interaction`、`model` 由 `DESKWAIFU_BUILD_CONTROL` 控制。`model` 没有插件内的 `tests/` 目录，其页面与桌面行为分别由控制面板和 desktop 测试覆盖。

| 插件 | 测试源码 | 主要 CTest 目标 |
| --- | --- | --- |
| desktop | `plugins/desktop/tests/` | `model-catalog`、`model-hit-test`、`qt-window-input`、`interaction-processor`、`transform-editor` 等 |
| perception | `plugins/perception/tests/` | `perception-adapters`、`ocr-client`、`ocr-build`、`screenshot-service` |
| ipc | `plugins/ipc/tests/` | `ipc-transport`、`ipc-bounded-transport`、`command-processor`、`ctl` |
| reading_clip | `plugins/reading_clip/tests/` | `reading-pipeline` |
| speech | `plugins/speech/tests/` | `SpeechPlayer`、`MiniMaxSpeechSource`、`AudioPlayer`、`SpeechSettings`、`speech-contract-*` |
| system | `plugins/system/tests/` | `deskwaifu-launcher`；跨插件重启联动见 `platform/tests` 的 `deskwaifu-respawner` |
| interaction | `plugins/interaction/tests/` | `hotkey-panel` |
| model | 无插件内测试 | 页面：`platform/tests/gui` 的 `gui-tabs`；模型命令与资源：`plugins/desktop/tests/` |

跨插件的加载与流水线测试在 `platform/tests` 中注册（`speech-plugin-load`、`reading-plugin`、`processor-ownership`、`processor-pipeline`），控制面板整页验证在 `platform/tests/gui` 中注册（`gui-tabs`、`gui-plugin-shutdown`）。

先按根 [README](../README.md) 配置并构建，再运行需要的目标；`ctest --test-dir build -N` 可列出当前配置下实际注册的测试：

```bash
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON -DDESKWAIFU_BUILD_OCR=OFF
cmake --build build --parallel "$(nproc)"
ctest --test-dir build --output-on-failure -R '^(gui-tabs|hotkey-panel|reading-pipeline)$'
```
