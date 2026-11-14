# DeskWaifu Rust 重写

项目现已切换为根目录 Rust workspace，旧 C++/Qt 实现归档于 [`legacy_cpp/`](legacy_cpp/ARCHIVE.md)，不参与当前构建；不兼容旧插件 ABI。目前实现了进程内静态组合的插件生命周期、类型化发布订阅和周期任务。

## 目录与依赖方向

```text
./
├── apps/deskwaifu/           # 组合 runtime 与插件，目前是自测宿主
├── crates/
│   ├── sdk/                 # Plugin、Context、Message 等共享契约
│   └── runtime/             # 生命周期、有界 FIFO、单线程调度与计时
└── plugins/test-plugin/      # 只依赖 SDK；每秒发消息并核验自订阅
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

SDK 不引入 Qt、GPUI、Tokio 或其他执行器依赖。尚未实现 capability 请求/响应、deadline/取消、运行时插件加载、工作线程投递、IPC、渲染快照交换和生产级常驻宿主。

## 运行测试插件

在仓库根目录运行：

```sh
cargo run -p deskwaifu
```

宿主运行约 3.2 秒。测试插件每秒发布一个带递增序号的消息，并验证自己收到的序号；下一次 tick 或停止时发现遗漏会返回错误。正常情况下输出三组发送/接收记录及 `sent: 3, received: 3`。这是有限时长自测程序，不是完整桌宠后端。

## 检查

```sh
cargo fmt --all --check
cargo test --workspace
cargo clippy --workspace --all-targets -- -D warnings
```

测试使用手动推进的逻辑时钟检查每秒发送，不依赖实际 sleep。

## 后续迁移约束

- capability 请求需要单独表达请求 ID、截止时间、取消和不可用等结果；取消不等同于底层工作已停止。
- 控制命令使用有界队列；渲染状态和帧采用 latest-value 快照，不能积压旧帧。
- OCR 等阻塞 FFI 由专用 worker 拥有；逻辑超时不能提前释放仍在执行的资源。
- 控制面板与后端通过 IPC 解耦，UI 与 transport 类型不能渗入领域契约。
- 先验证窗口/layer-shell、Cubism 渲染和真实 FFI 的需求，再选择 UI 与执行器。若需要动态插件，明确设计 C ABI 或进程协议，不能直接跨动态库传 Rust trait object。

## Arch 安装包与 CI

CI 使用专用 `deskwaifu-rust` runner，执行格式检查、Clippy、测试、release 编译和 Arch 打包。`Containerfile` 不包含 Qt 或专有 Cubism SDK。运行 `bash scripts/rebuild-ci-image.sh` 构建、验收并私下传输镜像，按输出的镜像 ID 更新 runner。发布构建保持非 root makepkg，产出单个 `deskwaifu-bin` 包；详情见 [打包说明](packaging/release/README.md)。

`docs/`、`计划/`、`research/` 保留为历史设计参考，其中 C++/Qt 路径与功能不代表当前实现。第三方源码和 AUR 子模块暂留，但不参与当前 Rust 编译。
