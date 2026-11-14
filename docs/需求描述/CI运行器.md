# DeskWaifu Rust CI 运行器

当前根项目为 Rust workspace。历史 C++ 构建、Qt/Wayland 门禁与 ccache 方案不再适用于本分支。

## 镜像与标签

- `Containerfile` 使用 Arch `base-devel` 与系统 `rust` 包（含 Cargo、rustfmt、Clippy），另外安装 Git、Python、Node 与 libarchive。
- 当前不复制 Cubism SDK、GLEW 或应用源码，不安装 Qt/OCR 依赖。
- `bash scripts/rebuild-ci-image.sh` 构建、验收工具链并私下传输到 `${DESKWAIFU_RUNNER_HOST:-st}`，输出 `deskwaifu-rust:docker://sha256:...`。
- runner 配置位于 `st:~/.config/deskwaifu-ci-runner/config.yaml`；保留旧 `deskwaifu-arch` 标签供历史分支使用，只新增/更新 Rust 标签。
- 保持 `force_pull: false`、`docker_host: "-"`、`privileged: false`，不暴露宿主 Docker socket。

## 门禁

`.gitea/workflows/ci.yml` 定义 Rust test 与 Arch package 并行任务，统一汇总失败；评审工作流仍独立运行。

- test：cargo fmt、clippy、test、release build，以及发布与评审工具的 Python 测试。
- package：按当前 commit 归档源码，以非 root 用户运行 makepkg，验证单一 `deskwaifu-bin` 安装包及校验和。
- Cargo 构建使用 `--frozen`。当前只有 workspace 内依赖；引入 crates.io 依赖时需同时设计 vendoring，不能静默解除离线限制。
- 发布流水线保留精确标签校验、隔离构建和安全上传机制，详见 [打包说明](../../packaging/release/README.md)。

## 运维边界

备份配置，确认无活跃任务后，仅重启 `deskwaifu-ci-runner.service`，不重启评审服务。保留原 CPU/内存、容量与卷挂载设置，不因本次语言迁移调整资源配额。

固定镜像检查通过、私下传输成功、runner 服务健康、本地容器门禁通过和远端 Actions 运行通过是不同证据，必须分别报告。
