# DeskWaifu Rust 重写

这是与现有 C++/Qt 项目并行的独立 Rust workspace。当前仅建立代码边界，不实现迁移中的业务功能；既不替代顶层 CMake 构建，也不改变现有插件。

## 目录约定

```text
rust/
├── Cargo.toml                 # workspace 与共享 lint
├── apps/
│   └── deskwaifu/             # 后端宿主的组合入口
└── crates/
    ├── sdk/                   # 插件与宿主共享的类型/契约
    └── runtime/               # 消息路由、生命周期与宿主运行机制
```

后续领域模块按迁移需求逐步建立 crate（例如 desktop、perception、speech、reading-clip、IPC）；控制面板可作为独立 app。不要为了预留名字而创建空 crate。Cubism 或其他 C/C++ SDK 的适配应放在单独的 bridge crate，保持 unsafe/FFI 边界清晰。

## 依赖方向

```text
apps/deskwaifu → runtime → sdk
领域插件 crates → sdk
apps/deskwaifu → 领域插件 crates（装配时）
```

`runtime` 不应反向依赖具体领域插件；插件之间通过 SDK 中的类型化消息或 capability 契约协作，而非互相依赖实现。UI 与 IPC transport 也不应渗入领域契约。

## 从现有设计迁移时保留的语义

- 端点与插件上下文有明确生命周期；关闭时先使端点失效，再停止插件并等待其工作线程退出。
- capability 请求需表达 request ID、deadline、取消及不可用等结果；取消请求不等同于底层工作已停止。
- 控制命令使用有序、有界队列；渲染状态/帧使用 latest-value 快照，避免积压过期帧。
- OCR 等阻塞 FFI 工作由专属 worker 拥有；逻辑超时不能提前释放仍被任务使用的资源。
- 控制面板与后端继续通过独立 IPC 解耦。

这些是迁移约束，不是此骨架已实现的功能。选定具体执行器、UI toolkit 和动态插件策略前，应先验证窗口/layer-shell、Cubism 渲染线程及现有 FFI 的实际需求。不要将 Rust trait object 直接跨动态库当作稳定 ABI；可先静态组合 crate，只有确认需要第三方运行时加载后再设计明确的 C ABI 或进程协议。

## 检查

在仓库根目录运行：

```sh
cargo fmt --manifest-path rust/Cargo.toml --check
cargo test --manifest-path rust/Cargo.toml
```
