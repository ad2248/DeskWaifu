# Rust CI 镜像依赖

- 在 `Containerfile` 安装必需依赖，并在 `scripts/rebuild-ci-image.sh` 添加工具/编译验收；不得在 workflow 临时安装或把必需测试改可选。
- Arch 的 `rust` 包包含 Cargo、rustfmt 与 Clippy；构建后验证实际可执行工具，不猜测独立包名。
- 使用脚本构建并私下传输镜像。当前 Rust 镜像不含专有 SDK；将来若加入 Cubism，不得推送到公开 registry。
- 专用 runner 使用 `deskwaifu-rust:docker://sha256:<镜像ID>`，`force_pull: false`。保留旧标签供历史分支使用。
- 更新前备份 `st:~/.config/deskwaifu-ci-runner/config.yaml`，核对服务边界、无活跃任务后仅重启 `deskwaifu-ci-runner.service`，不得重启评审服务。
- 区分本地镜像检查、镜像传输、runner 部署及远端 CI 结果；未运行远端 CI 时不能声称 CI 已通过。
