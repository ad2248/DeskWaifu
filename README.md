# DeskWaifu Rust 重写

项目现已切换为根目录 Rust workspace，旧 C++/Qt 实现归档于 [`legacy_cpp/`](legacy_cpp/ARCHIVE.md)，不参与当前构建；不兼容旧插件 ABI。目前实现了进程内静态组合的插件生命周期、周期任务、插件命令注册和平台 JSON socket IPC。命令通路不使用消息广播；SDK/runtime 中原有类型化发布订阅接口暂时保留。

## 目录与依赖方向

```text
./
├── apps/deskwaifu/           # 常驻平台宿主 + IPC 命令行客户端
├── crates/
│   ├── sdk/                 # Plugin、Context、命令注册等共享契约
│   ├── runtime/             # 生命周期、命令派发、单线程调度与计时
│   └── os/                  # 平台链接的 Unix socket / JSON IPC 组件（不是插件）
└── plugins/test-plugin/      # 只依赖 SDK；注册 test 命令，返回收到的参数
```

依赖方向为 `app → runtime → sdk` 和 `app → plugin → sdk`。插件对 runtime 的依赖仅用于测试。runtime 不引用具体领域插件。后续按迁移需要新增 desktop、perception、speech 等 crate；控制面板作为独立 app，Cubism 等 FFI 放在独立 bridge crate，不预先创建空模块。

## SDK 与 runtime 的当前契约

- `Plugin::initialize` 登记订阅；所有插件初始化成功后统一激活，再依次 `start`，所有启动完成后才能派发消息。
- `Context::subscribe::<T>()` 根据 Rust `TypeId` 订阅，重复订阅不会重复投递。`publish(T)` 广播给所有订阅者，包括发布者自己；返回成功仅代表入队，没有订阅者也成功。
- 消息负载为 `Arc<dyn Any + Send + Sync>`，接收方通过 `Message::payload::<T>()` 读取；附带发送者 `PluginId`。这是同一可执行程序中的类型标识，不是 IPC 序列化格式或稳定 ABI。
- 容量限制排队的广播条数，每条广播保存共享负载与投递目标；队列满返回 `QueueFull`，整个广播不会部分入队。不限制消息字节数或插件内部缓冲。
- 回调在调用 runtime 的线程上串行执行，发布不会递归调用接收者。每次 `advance` 最多派发容量数量的广播，防止自发消息让一次调度永不结束；排队广播按 FIFO、接收者按注册顺序派发。
- `tick_interval` 在初始化后读取，必须非零。首次 tick 在一个周期后执行；落后时跳过错过的 tick，不追赶。回调应短小，阻塞工作应交给插件拥有的 worker；当前 Context 不能传到工作线程发布消息。
- `advance(Duration)` 推进逻辑时钟，方便测试不等待真实时间；`run_for(Duration)` 以单调时钟运行有限时长。不要在同一次运行中混用手动推进和外部真实时间假设。
- 初始化、启动、tick 或消息处理返回错误时，宿主使所有端点失效并丢弃排队消息，再逆序停止所有尝试过初始化的插件。停止某个插件失败不会阻止其余插件清理；不会捕获 panic。
- 显式 `stop()` 可取得清理错误，可重复调用；Drop 仅兜底清理并忽略错误。停止后的 runtime 不可重启，插件应在 stop 中等待自己的 worker 结束。

- `Context::register_command(name, help, callback)` 在初始化期间注册子命令，`help` 为字符串数组，回调接收子命令后的 `&[String]` 并返回输出或错误。重复命令和保留的 `help` 被拒绝。内建 `help` 对齐显示所有已注册命令及多行帮助。

SDK 不引入 Qt、GPUI、Tokio 或其他执行器依赖。尚未实现 capability 请求/响应、deadline/取消、运行时插件加载、工作线程投递、渲染快照交换和完整桌宠后端。

## 运行平台与测试插件

直接运行 `deskwaifu` 启动常驻主程序（当前 IPC 仅支持 Unix 系统）；在另一个终端运行 `deskwaifu test` 调用测试插件，`deskwaifu help` 查看所有已注册子命令。主程序与 CLI 使用同一个可执行文件，不需要额外的 `serve` 子命令。

在源码目录中等价的 Cargo 用法：

```sh
# 终端一：启动主程序
cargo run -p deskwaifu
```

```sh
# 终端二：调用插件子命令
cargo run -p deskwaifu -- test
cargo run -p deskwaifu -- test '你好 world' '' '001'
cargo run -p deskwaifu -- help
```

socket 地址固定为 **`/tmp/deskwaifu.socket`**，不读取 `XDG_RUNTIME_DIR`，也不提供 `--socket` 配置。`deskwaifu test ...` 将 `test` 及其后的参数编码成 CLI 请求，通过该 socket 发送给已运行的主程序，再由 runtime 调用插件注册的回调。客户端不会自己加载插件或回退到本地派发；主程序未启动时命令调用返回错误。`--help` 显示启动器用法，无需启动主程序。

socket 权限为 0600。已有路径不会被覆盖或自动删除，包括崩溃后遗留的 socket；应确认无存活服务后再自行清理。再次无参数启动时若地址已占用，会报错而不会替换原有监听器。Ctrl+C 正常退出会停止插件并移除当前主程序自己的 socket；强制终止（SIGKILL）不保证清理。固定路径意味着同一系统同一时间只能有一个实例绑定此地址。

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

IPC 不经过插件广播总线。平台拥有 socket 和 runtime，在同一主线程调用已注册的命令；`crates/os` 不依赖 SDK/runtime，插件无需了解传输协议。

## 检查

```sh
cargo fmt --all --check
cargo test --workspace
cargo clippy --workspace --all-targets -- -D warnings
```

runtime 测试使用手动推进的逻辑时钟；IPC 测试使用真实 Unix socket 验证原始参数、分帧、异常隔离与资源清理，应用测试启动真实服务进程并通过 CLI 调用。

## 后续迁移约束

- capability 请求需要单独表达请求 ID、截止时间、取消和不可用等结果；取消不等同于底层工作已停止。
- 控制命令使用有界队列；渲染状态和帧采用 latest-value 快照，不能积压旧帧。
- OCR 等阻塞 FFI 由专用 worker 拥有；逻辑超时不能提前释放仍在执行的资源。
- 控制面板与后端通过 IPC 解耦，UI 与 transport 类型不能渗入领域契约。
- 先验证窗口/layer-shell、Cubism 渲染和真实 FFI 的需求，再选择 UI 与执行器。若需要动态插件，明确设计 C ABI 或进程协议，不能直接跨动态库传 Rust trait object。

## Arch 安装包与 CI

CI 使用专用 `deskwaifu-rust` runner，执行格式检查、Clippy、测试、release 编译和 Arch 打包。`Containerfile` 不包含 Qt 或专有 Cubism SDK。运行 `bash scripts/rebuild-ci-image.sh` 构建、验收并私下传输镜像，按输出的镜像 ID 更新 runner。发布构建保持非 root makepkg，产出单个 `deskwaifu-bin` 包；详情见 [打包说明](packaging/release/README.md)。

`docs/`、`计划/`、`research/` 保留为历史设计参考，其中 C++/Qt 路径与功能不代表当前实现。第三方源码和 AUR 子模块暂留，但不参与当前 Rust 编译。
