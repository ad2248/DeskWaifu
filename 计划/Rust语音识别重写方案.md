# Rust 版语音识别重写方案

## 交付目标

在 Rust 宿主中增加 `voice_input` 动态插件。用户可用合成器快捷键调用 `deskwaifucli start_voice_input`，按配置时长录音，到时自动开始云端识别，识别完成后从 Wayland 剪贴板手动粘贴。插件提供录音时长配置、状态查询和复制重试，并与 TTS 分开配置。 [旧版使用说明](<../docs/语音输入.md#L3>) · [当前宿主架构](<../关键设计/程序架构.md#L75>)

| 阶段 | 范围 | 可交付结果 |
| --- | --- | --- |
| 首批可用版本 | CLI、录音、百炼 WSS、Wayland 剪贴板、文件配置 | 新插件可随宿主打包和安装，快捷键链路可在无焦点桌面使用 |
| 旧协议兼容 | 原生 HTTPS 和兼容模式 HTTPS | 用户可以迁移原有端点和模型，协议分支分别通过离线测试 |
| 补齐旧版入口 | 控制面板与桌面通知、进程内独立 PCM 转录 | 控制面板支持配置和录音操作，其他插件可只取转录且无需复制 |

首批支持 Linux Wayland。控制面板依赖 Rust GUI 接入。独立 PCM 转录依赖目标明确的 capability 请求、deadline 和取消，等有具体调用方再接入。历史实施计划描述了早期 HTTP 设计。默认协议以归档代码已实现的 WSS 为准。 [历史实施计划](<语音识别功能.md#L57>) · [旧版默认配置](<../legacy_cpp/plugins/voice_input/VoiceInputSettings.hpp#L6>)

## 宿主接入与并发模型

- 在 `plugins/voice-input` 单独构建 Rust `cdylib`，通过现有 SDK 注册命令。当前回调是 `fn(&[String]) -> Result<String, String>`。建议由注册表保存命令元数据和所属 `PluginId`，runtime 以 `&mut self` 分发到所属插件的 `on_command`，由插件实例访问控制器。迁移测试插件和派发调用点，保持 `help`、参数原样传递、命令冲突检查和初始化失败回滚。同步重建宿主与 `.so`。 [SDK 命令契约](<../crates/sdk/src/lib.rs#L22>) · [runtime 派发](<../crates/runtime/src/lib.rs#L214>)
- 框架通过 `Context` 提供可持有的 OS 服务句柄，并跟踪插件提交的异步任务。`on_command` 更新控制状态、提交任务后立即返回。任务持有服务句柄和会话所需数据，逐块读取采集流并在流正常结束后启动 ASR，最后等待复制完成。等待期间宿主继续处理 CLI 命令。状态查询读取插件的当前快照。 [当前调度与生命周期](<../README.md#L26>)
- 语音插件同一时间运行一个会话。空闲时 `start_voice_input` 提交定时采集任务。采集、识别或复制进行中再次触发，返回 `busy`。平台资源清理完成后才释放录音槽。CLI 会话号用于结果查询和复制重试。

## 系统服务接口（待审核）

`crates/sdk` 定义接口和数据类型。`crates/os` 实现录音、PCM 转换和系统剪贴板，由宿主注入可持有的服务句柄。插件仅依赖 SDK，在同一个异步任务中依次 `await` 系统操作，直接取得各自的结果。语音插件同一时间只执行一条采集 → 识别 → 复制流程。其他插件使用同一个 OS 服务，麦克风和剪贴板各自由 OS 串行访问。 [现有 Context](<../crates/sdk/src/lib.rs#L89>) · [平台宿主事件循环](<../crates/os/src/host.rs#L10>)

```rust
use futures_core::Stream;
use std::{future::Future, pin::Pin, rc::Rc, time::Duration};

type LocalFuture<'a, T> = Pin<Box<dyn Future<Output = T> + 'a>>;
type AudioStream = Pin<Box<dyn Stream<Item = Result<Vec<u8>, OsError>>>>;

trait OsServices {
    /// 启动采集并返回音频流。每块为 16 kHz、单声道、有符号 16 位小端 PCM。
    /// 块大小可变，每块包含完整样本。`duration` 从设备启动时开始计算。
    fn capture_audio(&self, duration: Duration) -> LocalFuture<'_, Result<AudioStream, OsError>>;
    fn read_clipboard_text(&self) -> LocalFuture<'_, Result<Option<String>, OsError>>;
    fn write_clipboard_text(&self, text: String) -> LocalFuture<'_, Result<(), ClipboardWriteError>>;
}

// Context::os(&self) -> Rc<dyn OsServices>

enum ClipboardWriteError {
    Failed(OsError),  // 确认本次写入未生效
    Unknown(OsError), // 无法确认本次写入是否生效
}
```

- `capture_audio(...).await` 在设备启动后返回流，插件在录音期间用 `stream.next().await` 按顺序读块。达到配置时长时，OS 停止采集，交付截止点前已接收的尾部数据，清理设备并结束流。启动失败由外层 Future 返回，采集失败作为流的最后一项返回。OS 按平台策略选择设备，负责 PCM 转换。内部队列有界，队列溢出时结束采集并报告错误。
- 剪贴板首版支持 UTF-8 文本，建议使用 1 MiB 上限。`read_clipboard_text().await` 用 `None` 表示无文本，用 `Some("")` 表示空文本。`write_clipboard_text(text).await` 在确认写入后返回 `Ok(())`。取消期间已提交的系统写入可能仍然生效。OS 管理操作时限、进程回收和写入后的 selection。
- SDK 用装箱 Future 支持通过 `dyn OsServices` 注入实现或测试替身。宿主在现有单线程异步上下文中运行并跟踪插件任务，任务持有服务句柄，不跨 `await` 借用 `Context` 或整个插件实例。停止时宿主先取消并等待插件任务结束，再卸载动态库。OS 在启动 Future 或采集流被丢弃时负责后台清理，原操作释放设备后才能接受下一次采集。CLI 会话号仅用于现有语音命令。系统操作的结果由各自的 `await` 返回。

## 录音、识别与剪贴板

| 模块 | 实现要点 |
| --- | --- |
| `os::audio_input` | 先用 CPAL 验证 Arch/Linux 的编译依赖、PipeWire 会话下的虚拟麦采集及断开行为，再确定后端。若该组合无法通过虚拟麦采集验证，比较直接连接 PipeWire 的实现。音频回调使用有界缓冲，格式转换由 OS 执行。语音插件对录音时长的默认值为 60 秒，可配置 1–300 秒。按 16 kHz 单声道计算，300 秒对应 9,600,000 字节。 |
| `asr` | 百炼 WSS 默认模型为 `qwen-audio-3.1-asr-flash-streaming`。停止录音后连接，发送 `run-task` 并等待 `task-started`，再按旧版节奏每 100 ms 发送 3200 字节 PCM，末尾补 250 ms 静音后发送 `finish-task`。校验 `task_id` 和事件顺序，忽略心跳与中间假设，只累计最终句子。收到 `task-finished` 后交付完整文本。以旧版 15 秒启动时限、120 秒加发送时长的总时限、1 MiB 消息和文本上限作为兼容起点。上传通道支持取消，失败后结束本次会话。 |
| `os::clipboard` | 首版通过 `wl-paste` 和 `wl-copy` 适配 Wayland。异步读取有界输出，写入时将原始 UTF-8 送入 stdin、关闭 stdin 并等待后端确认。成功后验证 selection holder 持续供给文本。超时或取消时回收仍在处理的进程。 |

参考 [录音实现](<../legacy_cpp/plugins/voice_input/private/audio/AudioCapture.cpp#L20>) · [WSS 实现](<../legacy_cpp/plugins/voice_input/private/asr/NativeAsrClient.cpp#L200>) · [剪贴板后端](<../legacy_cpp/plugins/desktop/private/clipboard/ClipboardService.cpp#L142>)。

HTTPS 兼容沿用旧版协议选择。原生请求使用 WAV/base64、`X-DashScope-SSE: disable` 和字符串 `sample_rate`，响应优先读 `text`，其次读 `output.text`。兼容模式将 `/v1` 补全为 `/chat/completions`，读取 `choices[0].message.content`。旧模型 `qwen-audio-3.0-asr-flash` 用同域原生路径，上传前完成解析。生产连接使用 WSS 或 HTTPS，关闭携带凭据的重定向及自动重传。空白转录返回 `no_speech`，合法结果保留原始空格和换行。 [旧版协议实现](<../legacy_cpp/plugins/voice_input/private/asr/NativeAsrClient.cpp#L56>)

## CLI、配置与部署

- 首版提供 `start_voice_input`、`get_voice_input_status`、`get_voice_input_result --session_id ID`、`retry_voice_input_copy --session_id ID` 和 `get_voice_input_settings`。现有 IPC 信封保持原样，成功的业务输出可用 JSON 字符串表示。命令拒绝映射为 CLI 非零退出。状态返回字符串会话 ID、阶段、时长、错误码和复制结果。转录全文通过显式结果查询获取。旧版快捷键改绑 `deskwaifucli start_voice_input`。 [旧版命令](<../docs/语音输入.md#L26>) · [现有 IPC](<../README.md#L71>)
- 单条 IPC 响应上限为 64 KiB，测试按外层 JSON 转义后的字节数核算。显式结果查询对超限文本返回 `result_too_large_for_ipc`，并保留会话内完整文本供后台复制。状态、复制重试和 PCM 传输均不依赖大帧。 [IPC 上限](<../README.md#L81>)
- 首批通过独立的用户配置文件设置 `endpoint`、`model`、`max_recording_seconds` 和 `api_key`。文件使用 `0600`，公开设置只显示密钥配置状态。通过后台刷新和校验，修复配置后下一次会话即可使用，新设置不改变进行中会话。提供旧配置手工迁移说明，迁移 ASR 和录音时限设置。后续控制面板接入时补配置写入命令和原子保存。原始录音和转录默认驻留内存，普通日志不包含音频、转录或密钥。 [旧版配置语义](<../docs/语音输入.md#L24>)
- 更新 workspace、开发构建脚本和 Arch 安装模板，使同批构建的插件进入宿主 `plugins/` 目录。按音频后端验证结果更新 CI 镜像和系统依赖，保留 release 离线编译路径。安装冒烟测试应从安装后的 CLI 验证插件命令可发现。 [开发构建脚本](<../scripts/build.sh#L19>) · [Arch 安装模板](<../packaging/release/PKGBUILD.in#L19>)

## 实施次序与验收

1. **确认两个平台接入点。** 用候选音频后端采集 PipeWire 虚拟麦，验证固定目标格式、结束时的尾部标记和设备断开。用独立 `wl-paste --no-newline` 验证无焦点环境里的 `wl-copy` 结果，包括中文、换行和前后空白。将实际支持的合成器和系统依赖写入使用文档。
2. **接通 SDK、系统服务与 CLI。** 注入 fake OS 服务，验证异步调用按顺序完成、插件停止后清理在途操作，以及剪贴板无文本、空文本、超限、写入失败和写入结果未知。用可控时钟验证录音期间可读取 PCM 块、到时交付尾部并结束流，以及队列溢出后跳过识别。通过真实宿主进程和动态插件走 Unix socket，验证采集或识别期间仍可查询状态，再次触发时返回 `busy`。插件任务的预期错误进入会话状态，宿主继续接收其他命令。
3. **接入生产录音、WSS 与复制。** 用环回 WSS mock 核对 `run-task`、分块 PCM、250 ms 尾静音、最终句合并、错误事件和超时。私有 PipeWire 与 headless Sway 进行全链路冒烟，虚拟麦使用合成音频，剪贴板由独立进程读回。验证到时自动停止并触发一次识别、正常复制、识别失败时原剪贴板保持不变，以及复制失败后的会话内重试。最后验证 Arch 包可安装、启动和发现插件。
4. **补齐兼容协议和用户入口。** 用环回 mock 覆盖两个 HTTPS JSON 形态、同域路径解析与拒绝重定向。在控制面板可用后加入凭据编辑、会话状态和通知。进程内能力使用独立 PCM 请求与录音共享任务槽，验证只取文本、请求超时与取消。

普通自动测试使用 fake 或本地 mock，使用显式的集成环境验证虚拟音频和 Wayland。云端验收单独使用已知短句的合成音频，核对关键词并确认配置账户的模型可用性。两类验收分别记录结果。 [旧版集成验证](<../legacy_cpp/plugins/voice_input/tests/README.md#L41>) · [合成语音夹具](<../legacy_cpp/plugins/voice_input/tests/fixtures/README.md>)
