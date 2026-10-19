---
name: ci-image-dependencies
description: CI 因缺少必需依赖失败时，补齐 DeskWaifu 镜像并更新 runner 的固定镜像引用。
---

- 在 `Containerfile` 补依赖，同时将新增模块加入 `scripts/rebuild-ci-image.sh` 的镜像验收；不要靠 workflow 临时安装或将必需依赖改成可选来绕过。
- 用 `scripts/rebuild-ci-image.sh` 重建并私有传输镜像；镜像含 Cubism SDK，不得推送到公开 registry。
- 更新专用 runner 的 `deskwaifu-arch:docker://sha256:<新镜像ID>`，保持 `force_pull: false`，重启该 runner。只改 Containerfile 不会更新固定镜像。
- 重跑原 PR 的 CI，核对执行日志使用新镜像 ID，且原先缺失的模块已被找到。
