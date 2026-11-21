# DeskWaifu Rust CI 运行器

当前根项目为 Rust workspace。历史 C++ 构建、Qt/Wayland 门禁与 ccache 方案不再适用于本分支。

## 镜像与标签

- `Containerfile` 使用 Arch `base-devel` 与系统 `rust` 包（含 Cargo、rustfmt、Clippy），另外安装 Git、Python、Node、libarchive、CMake、GLVND 和 Mesa。
- 私有镜像从本地构建上下文复制 `CubismSdkForNative-5-r.5` 的 Core、Framework、许可说明，以及 Haru、Hiyori、Mao 三个示例模型。镜像通过 `CUBISM_SDK_DIR` 和 `CUBISM_GLEW_DIR` 提供 SDK 与 GLEW 源码路径。
- `bash scripts/rebuild-ci-image.sh` 在断网容器里验证工具链、构建原生 `terminal`、运行离屏测试并渲染三个模型。验收通过后通过 SSH 传输到 `${DESKWAIFU_RUNNER_HOST:-st}`，输出 `deskwaifu-rust:docker://sha256:...`。包含专有 SDK 的镜像仅在私有环境使用。
- runner 配置位于 `st:~/.config/deskwaifu-ci-runner/config.yaml`；保留旧 `deskwaifu-arch` 标签供历史分支使用，只新增/更新 Rust 标签。
- 保持 `force_pull: false`、`docker_host: "-"`、`privileged: false`，不暴露宿主 Docker socket。

## 门禁

`.gitea/workflows/ci.yml` 定义 Rust test 与 Arch package 并行任务，统一汇总失败；评审工作流仍独立运行。

- test：cargo fmt、clippy、test、release build，以及发布与评审工具的 Python 测试。
- package：按当前 commit 归档源码，以非 root 用户运行 makepkg，验证单一 `deskwaifu-bin` 安装包及校验和。
- 镜像构建通过 `cargo vendor --locked` 收集依赖。Rust CI 的检查与 Arch 打包只读取镜像内的 Cargo 源。锁文件新增或更新外部依赖时，先用对应代码重建镜像并更换 Runner 镜像 ID。
- 缓存使用 runner 已挂载的 `/cache`。Rust 检查使用 `/cache/sccache-rust-test`，Arch CI 打包使用 `/cache/sccache-rust-package`，各限 4 GiB。每个目录同时只运行一个 sccache 服务，抢锁失败或缓存不可用时直接编译。构建日志包含缓存命中统计。
- 正式 Release 使用独立的隔离构建流程，禁用 CI 共享缓存。详见[打包说明](../../packaging/release/README.md)。

## 运维边界

备份配置，确认无活跃任务后，仅重启 `deskwaifu-ci-runner.service`，不重启评审服务。保留原 CPU/内存、容量与卷挂载设置，不因本次语言迁移调整资源配额。

固定镜像检查通过、私下传输成功、runner 服务健康、本地容器门禁通过和远端 Actions 运行通过是不同证据，必须分别报告。
