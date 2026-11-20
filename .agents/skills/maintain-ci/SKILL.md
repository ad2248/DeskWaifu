---
name: maintain-ci
description: 维护 DeskWaifu 的 Gitea Rust CI、Arch 打包和 Podman Runner。用于排查作业失败、依赖下载和编译缓存，以及更新或回退 Runner 镜像。
---

# 维护 DeskWaifu CI

## 排查

1. 按提交 SHA、run 和 job 找到失败步骤。排队时检查 Runner 服务、标签和可用槽位。PR 状态与自动评审沿用现有技能。 [Rust CI](<../../../.gitea/workflows/ci.yml>) · [PR 监控](<../watch-pr-ci/SKILL.md>) · [审查维护](<../gitea-pr-review-ci/SKILL.md>)
2. 卡在 crates.io 索引时，用 Runner 固定镜像检查 `cargo fetch --locked` 的代理。Arch 打包仅向非 root 的 fetch 阶段传递代理，编译保持离线。 [打包入口](<../../../scripts/release-build.sh>)
3. 排查缓存时核对 `/cache` 挂载、构建用户权限和命中日志。`.lock` 必须放在 `entries/` 外，防止被淘汰。抢锁失败直接编译，正式 Release 使用独立构建环境。 [缓存入口](<../../../scripts/ci-sccache.sh>)

## 部署

1. 按镜像依赖说明构建和验收。在 Runner 主机上使用服务用户的 rootless Podman。跨主机时走私有传输。 [镜像依赖说明](<../ci-image-dependencies/SKILL.md>)
2. 确认作业空闲并备份配置。只切换 `deskwaifu-rust` 的镜像 ID，重启专用服务并确认重新注册。切换失败时恢复备份并重启。 [Runner 运维说明](<../../../docs/需求描述/CI运行器.md>)

## 验收

1. 用固定镜像执行受影响的步骤，再核对 Gitea 上同一提交的作业结果。分别报告本地与线上结果。 [本机 CI 验证](<../local-ci-tests/SKILL.md>)
2. 改动缓存时，在共享缓存卷的两个独立容器内对同一提交跑冷、热构建，比较命中数、耗时和包 SHA-256，并验证安装与 CLI。线上重跑同一提交的打包作业确认命中。
