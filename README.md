# DeskWaifu

DeskWaifu 是面向 Linux Wayland 的桌宠应用。Rust workspace 提供宿主、命令行客户端、GPUI 控制面板和动态插件，支持 Live2D 模型显示与交互，以及定时录音、语音识别和结果复制。

## 目录与依赖方向

| 目录 | 职责 |
| --- | --- |
| [apps](<apps/>) | 宿主、IPC 客户端和 GPUI 控制面板 |
| [crates](<crates/>) | 插件 SDK、运行时、平台服务、Cubism 和 Wayland 适配 |
| [plugins](<plugins/>) | `terminal`、`voice-input` 和 `test-plugin` |

宿主通过 runtime 加载独立的 `.so`，不直接依赖具体业务插件。控制面板和 CLI 通过 Unix socket 调用宿主，插件进程内通信使用 SDK 的类型化发布订阅。详见[程序架构](<关键设计/程序架构.md>)和[文档索引](<docs/README.md>)。

## SDK 与 runtime 的当前契约

- `Plugin::initialize` 登记订阅；尝试初始化所有插件后统一激活通信，只对初始化成功的插件调用 `start`，所有启动完成后才能派发消息。
- `Context::subscribe::<T>()` 根据 Rust `TypeId` 订阅，重复订阅不会重复投递。`publish(T)` 广播给所有订阅者，包括发布者自己；返回成功仅代表入队，没有订阅者也成功。
- 消息负载为 `Arc<dyn Any + Send + Sync>`，接收方通过 `Message::payload::<T>()` 读取；附带发送者 `PluginId`。这是同一可执行程序中的类型标识，不是 IPC 序列化格式或稳定 ABI。
- 容量限制排队的广播条数，每条广播保存共享负载与投递目标；队列满返回 `QueueFull`，整个广播不会部分入队。不限制消息字节数或插件内部缓冲。
- 回调在调用 runtime 的线程上串行执行，发布不会递归调用接收者。每次 `advance` 最多派发容量数量的广播，防止自发消息让一次调度永不结束；排队广播按 FIFO、接收者按注册顺序派发。
- `tick_interval` 在初始化后读取，必须非零。首次 tick 在一个周期后执行；落后时跳过错过的 tick，不追赶。回调应短小，阻塞工作应交给插件拥有的 worker；当前 Context 不能传到工作线程发布消息。
- `advance(Duration)` 推进逻辑时钟，方便测试不等待真实时间；`run_for(Duration)` 以单调时钟运行有限时长。不要在同一次运行中混用手动推进和外部真实时间假设。
- 初始化失败时，宿主记录插件名和错误，撤销该插件的命令与订阅，调用 `stop` 清理部分初始化的资源并跳过该插件。
- 启动、tick 或消息处理返回错误时，宿主使所有端点失效并丢弃排队消息，再逆序停止已初始化的插件。停止某个插件失败后仍会清理其余插件。插件 panic 会继续传播。
- 显式 `stop()` 可取得清理错误，可重复调用；Drop 仅兜底清理并忽略错误。停止后的 runtime 不可重启，插件应在 stop 中等待自己的 worker 结束。

- `Context::register_command(name, help)` 在初始化期间登记命令名与帮助。运行时将命令分派到所属插件实例的 `Plugin::on_command`，参数原样传入。重复命令和保留的 `help` 被拒绝。内建 `help` 对齐显示所有已注册命令及多行帮助。

`apps/deskwaifu` 仅负责启动主线程上的 Tokio `current_thread` 异步上下文、加载插件并调用 `deskwaifu_os::run`。`crates/os` 负责绑定和监听 socket、启动与停止插件，在该主线程上执行异步主循环、插件回调与 IPC 命令派发；按 socket 就绪事件处理 IPC，并根据插件下一次到期时间推进插件时钟；无消息、无周期任务时挂起等待，繁忙时跳过错过的 tick，收到 SIGINT / SIGTERM 后停止插件并清理 socket。插件回调仍为同步接口，不应阻塞主线程。

`Context::spawn_local` 提交异步任务，由宿主在主线程驱动。退出时，运行时先丢弃未完成的任务，再调用插件的 `stop`。SDK 与 runtime 本身不依赖 Tokio。

## 运行平台与测试插件

直接运行 `deskwaifu` 启动常驻主程序（当前 IPC 仅支持 Unix 系统）；在另一个终端运行独立程序 `deskwaifucli test` 调用测试插件，`deskwaifucli help` 查看所有已注册子命令。主程序完全不接受任何参数，包括 `--help`；CLI 的 `--help` 无需启动主程序。

从仓库根目录直接用 Cargo 构建宿主、CLI、三种插件和 GPUI 控制面板。完整构建需要近期 Rust、PipeWire/Clang、Cubism SDK/GLEW、CMake、OpenGL/EGL、libsystemd 和面板开发库。先按 [Cubism 构建说明](<crates/cubism-bridge/README.md#L3>) 设置 `CUBISM_SDK_DIR` 和 `CUBISM_GLEW_DIR`。

```sh
cargo build --workspace --locked
./scripts/deploy-plugins.sh
# 终端一：启动主程序
./target/debug/deskwaifu
```

用 `cargo build --workspace --locked --release` 构建 Release，再运行 `./scripts/deploy-plugins.sh --release`。部署辅助脚本不调用 Cargo，只把已有的 `.so` 复制到对应 profile 的 `plugins/`；使用自定义路径时，部署步骤也要设置相同的 `CARGO_TARGET_DIR`。不要混用不同构建的主程序和插件。面板可运行 `./target/debug/deskwaifu-terminal-panel`；模型目录和启动模型通过共享配置指定。 [terminal 用法](<plugins/terminal/README.md>)

```sh
# 终端二：调用插件子命令
cargo run -p deskwaifucli -- test
cargo run -p deskwaifucli -- test '你好 world' '' '001'
cargo run -p deskwaifucli -- help
cargo run -p deskwaifucli -- --help
```

socket 地址固定为 **`/tmp/deskwaifu.socket`**，不读取 `XDG_RUNTIME_DIR`，也不提供 `--socket` 配置。`deskwaifucli test ...` 将 `test` 及其后的参数编码成 CLI 请求，通过该 socket 发送给已运行的主程序，再由 runtime 调用插件注册的回调。客户端不会自己加载插件或回退到本地派发；主程序未启动时命令调用返回错误。`deskwaifucli --help` 显示 CLI 用法，无需启动主程序。

socket 权限为 0600。已有路径不会被覆盖或自动删除，包括崩溃后遗留的 socket；应确认无存活服务后再自行清理。再次无参数启动时若地址已占用，会报错而不会替换原有监听器。Ctrl+C 正常退出会停止插件并移除当前主程序自己的 socket；强制终止（SIGKILL）不保证清理。固定路径意味着同一系统同一时间只能有一个实例绑定此地址。

### Linux 麦克风与 Wayland 剪贴板

插件通过 `Context::os()` 获取宿主共享的异步系统服务句柄。Linux 使用 PipeWire 会话的默认麦克风，按指定时长采集 16 kHz 单声道 S16LE PCM。剪贴板使用 `wl-clipboard` 的 `wl-copy` 和 `wl-paste` 读写 UTF-8 文本，单次上限为 1 MiB。运行宿主时需保留当前用户的 `XDG_RUNTIME_DIR` 和 `WAYLAND_DISPLAY`。Wayland 无焦点读写已在 Sway 1.10.1 和 wl-clipboard 2.2.1 上验证。

Linux 编译需要 `pkg-config`、PipeWire 开发头文件和可供 bindgen 加载的 libclang（Arch 为 `pkgconf libpipewire clang`，Debian 为 `pkg-config libpipewire-0.3-dev libclang-dev`）。录音需要 PipeWire 守护进程及 WirePlumber 等会话管理器。可用 `wpctl status` 检查默认输入源。

使用 `deskwaifucli start_voice_input` 启动定时录音。配置路径、命令和快捷键见[语音输入使用说明](<docs/语音输入.md>)。

### 动态插件部署与信任边界

runtime 从**实际可执行文件所在目录**下的 `plugins/*.so` 加载插件，按文件名排序，不依赖当前工作目录；Linux 上 `std::env::current_exe()` 会解析启动软链接。开发布局为 `target/debug/deskwaifu` + `target/debug/plugins/libdeskwaifu_test_plugin.so`（release 同理）。Arch 包安装实际程序到 `/usr/lib/deskwaifu/deskwaifu`，插件到 `/usr/lib/deskwaifu/plugins/`，`/usr/bin/deskwaifu` 为指向实际程序的软链接，独立 CLI 安装到 `/usr/bin/deskwaifucli`。

测试插件以 `rlib` 和 `cdylib` 构建，动态入口为普通 **Rust ABI** 的 `deskwaifu_plugin_create() -> Box<dyn Plugin>`。这不是稳定 C ABI，也没有版本兼容检查：主程序与所有插件必须使用**相同 Rust 工具链、同一份代码一起构建**。只加载可信插件，插件拥有宿主进程的权限，不能把此机制当成沙箱或第三方兼容接口。插件仅在启动时加载，退出清理后卸载，不支持热更新；替换插件后需重启主程序。

### JSON socket 私有协议

传输为 Unix stream socket；UTF-8 JSON 以 LF（`\n`）分帧，每行一个完整请求/响应。支持拆包、粘包及长连接，响应与请求同序；无需请求 ID。换行出现在 JSON 字符串中时必须转义。

请求示例（行末还需 LF）：

```json
{"source":"cli","payload":["test","你好 world","","001"]}
```

- `source` 目前只接受 `cli`；`gui` 及其他来源返回错误，不实现 GUI 负载或处理逻辑。
- `payload` 必须是字符串数组，第一个元素为子命令（不含可执行文件名），后续元素原样交给回调。不 trim、不按空格拆分，不解释为 shell 或继续解析正文 JSON。
- 信封字段严格校验，不接受未知字段。空数组由命令派发层返回缺少命令错误。
- 成功响应：`{"status":"ok","output":"命令输出"}`。
- 失败响应：`{"status":"error","error":"错误描述"}`。CLI 成功打印输出并退出 0，协议或命令错误退出非 0。
- 单条请求/响应最大 64 KiB（不含末尾 LF，按编码后的 JSON 字节计数）。超长请求返回错误并关闭连接，超长命令输出改为错误响应，不截断输出或执行超长请求。
- 无效 JSON/UTF-8/信封返回错误，不派发命令；仍可继续发送下一条完整请求。EOF 前没有 LF 的半帧不执行，返回错误后关闭连接。
- 最多同时维护 32 个连接；连接空闲读写超时 10 秒。平台以非阻塞方式有界读写，每连接每轮最多派发一条命令，避免某个半帧或不读响应的客户端阻塞整个宿主。回调本身仍需短小，不提供执行超时或取消。

IPC 不经过插件广播总线。`crates/os` 的平台宿主拥有 socket 和 runtime，在同一主线程调用已注册的命令；IPC 协议处理不引用插件 SDK，插件无需了解传输协议。

## 检查

```sh
cargo fmt --all --check
cargo build --workspace --locked
cargo test --workspace --locked
cargo clippy --workspace --all-targets --locked -- -D warnings
python3 -B -m unittest discover -s scripts -p 'test_*.py' -v
```

runtime 测试使用手动推进的逻辑时钟。宿主冒烟测试启动真实服务进程，通过 CLI 调用动态插件。

## Arch 安装包与 CI

CI 使用专用 `deskwaifu-rust` runner，执行格式检查、Clippy、测试、release 编译和 Arch 打包。私有 CI 镜像内置 Cubism Native SDK 5-r.5、GLEW 及 Haru、Hiyori、Mao 模型。运行 `bash scripts/rebuild-ci-image.sh` 构建镜像、离线验收 Live2D 并通过 SSH 私下传输，按输出的镜像 ID 更新 runner。发布构建保持非 root makepkg，直接安装 Cargo 产物，生成包含原生 `terminal` 和控制面板的 `deskwaifu-bin` 包。详情见 [打包说明](packaging/release/README.md)。
