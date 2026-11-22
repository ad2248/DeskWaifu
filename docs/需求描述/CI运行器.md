# DeskWaifu Rust CI 运行器

CI 作业在固定的私有 Arch 镜像中构建 Cargo workspace。

## 镜像与标签

- [Containerfile](../../Containerfile) 使用 Arch `base-devel` 与系统 `rust` 包（含 Cargo、rustfmt、Clippy），另外预装 CI 工具和原生依赖。Cubism 使用 CMake、GLVND 和 Mesa，KWin 光标桥使用 `systemd-libs` 的 sd-bus，GPUI 面板使用 Wayland、FreeType、Fontconfig、XKB Common（含 X11）、XCB、X11 和 Vulkan loader。
- 私有镜像从本地构建上下文复制 `CubismSdkForNative-5-r.5` 的 Core、Framework、许可说明，以及 Haru、Hiyori、Mao 三个示例模型。镜像通过 `CUBISM_SDK_DIR` 和 `CUBISM_GLEW_DIR` 提供 SDK 与 GLEW 源码路径。
- [镜像重建脚本](../../scripts/rebuild-ci-image.sh) 在断网容器里验证工具链、构建根工作区、检查面板产物和原生 `terminal` 插件的链接依赖、运行离屏测试并渲染三个模型。验收通过后通过 SSH 传输到 `${DESKWAIFU_RUNNER_HOST:-st}`，输出 `deskwaifu-rust:docker://sha256:...`。包含专有 SDK 的镜像仅在私有环境使用。
- 在 `st:~/.config/deskwaifu-ci-runner/config.yaml` 中将 `deskwaifu-rust` 标签指向通过验收的固定镜像，仅更新该标签。
- 保持 `force_pull: false`、`docker_host: "-"`、`privileged: false`，不暴露宿主 Docker socket。

## 无头 Plasma 会话

CI 镜像提供 KWin、Plasma Shell、桌面组件和 Mesa 软件渲染。CI 检查和镜像验收均通过启动脚本启动 1280×720 的虚拟输出，等待 Wayland socket 及 KWin、Plasma Shell 的 D-Bus 服务就绪。脚本为每次运行创建私有 D-Bus 会话和 XDG 目录。界面用例可以用 `bash scripts/with-headless-plasma.sh <命令>` 在这个会话中运行。 [会话启动脚本](<../../scripts/with-headless-plasma.sh>)

## 门禁

[Rust CI 工作流](../../.gitea/workflows/ci.yml) 定义 Rust test 与 Arch package 并行任务，统一汇总失败。评审工作流独立运行。

- `test` 直接以 `cargo build --workspace --frozen` 构建 debug 和 release（加 `--release`），包含 GPUI 控制面板和默认启用 `kwin-cursor`、`native` 的 `terminal` 插件。另运行 cargo fmt、Clippy、Rust 测试及 Python 工具测试。Cubism 离屏测试使用 Mesa 软件渲染，清除图形会话和硬件 PCI 选择变量。
- `package` 按当前 commit 归档源码，以非 root 用户运行 makepkg，验证单一 `deskwaifu-bin` 安装包及校验和。安装后核对 `deskwaifu-terminal-panel` 和全部插件动态库，由 CLI 验证宿主加载语音输入及 `terminal` 插件。
- 镜像构建通过 `cargo vendor --locked` 收集依赖。Rust CI 的检查与 Arch 打包只读取镜像内的 Cargo 源。锁文件新增或更新外部依赖时，先用对应代码重建镜像并更换 Runner 镜像 ID。
- 缓存使用 runner 已挂载的 `/cache`。Rust 检查使用 `/cache/sccache-rust-test`，Arch CI 打包使用 `/cache/sccache-rust-package`，各限 4 GiB。每个目录同时只运行一个 sccache 服务，抢锁失败或缓存不可用时直接编译。构建日志包含缓存命中统计。
- 正式 Release 使用独立的隔离构建流程，禁用 CI 共享缓存。详见[打包说明](../../packaging/release/README.md)。

## 运维边界

备份配置，确认无活跃任务后，仅重启 `deskwaifu-ci-runner.service`。保持评审服务、CPU/内存、容量与卷挂载设置。

固定镜像检查通过、私下传输成功、runner 服务健康、本地容器门禁通过和远端 Actions 运行通过是不同证据，必须分别报告。
