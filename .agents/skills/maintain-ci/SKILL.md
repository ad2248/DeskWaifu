---
name: maintain-ci
description: 维护 DeskWaifu 的 Gitea Rust CI、Arch 打包和 Podman Runner。用于排查作业失败、依赖下载和编译缓存，以及更新或回退 Runner 镜像。
---

# 维护 DeskWaifu CI

## 排查

1. 按提交 SHA、run 和 job 找到失败步骤。`build` 失败时 `test` 会被跳过，这是预期。排队时检查 Runner 服务、标签和可用槽位。PR 状态与评审沿用现有技能。 [Rust CI](<../../../.gitea/workflows/ci.yml>) · [PR 监控](<../watch-pr-ci/SKILL.md>)
2. 缺少 Cargo 依赖时，按当前 Cargo 锁文件重建 Runner 镜像。镜像构建负责联网，`build` 只读取镜像内的 Cargo 源。 [Runner 运维说明](<../../../docs/需求描述/CI运行器.md>)
3. 排查缓存时核对 `/cache` 挂载、目录可写性和命中日志。`.lock` 必须放在 `entries/` 外，防止被淘汰。抢锁失败直接编译，正式 Release 使用独立构建环境。 [缓存入口](<../../../scripts/ci-sccache.sh>)
4. `test` 在握手步骤失败时先看 `/transfer/<run>-<sha>/ready.json`：目录不存在说明 `build` 没发布或挂载丢失；`sha` 不符说明指针落后；摘要不符说明包被改过。两个 Runner 配置里的 `-v ...:/transfer` 与「两个 Runner 同机」是前提，不要在作业里绕过。

## 部署

1. 按镜像依赖说明构建和验收。在 Runner 主机上使用服务用户的 rootless Podman。跨主机时走私有传输。 [镜像依赖说明](<../ci-image-dependencies/SKILL.md>)
2. 确认作业空闲并备份配置。分别切换 `deskwaifu-rust`、`deskwaifu-gui` 的镜像 ID，重启专用服务并确认重新注册。切换失败时恢复备份并重启。 [Runner 运维说明](<../../../docs/需求描述/CI运行器.md>)

## 验收

1. 用固定镜像复用工作流步骤。以 `--network none` 验证镜像内的依赖可以完成构建和打包。分别报告本地与线上的结果。 [本机 CI 验证](<../local-ci-tests/SKILL.md>)
2. 验收打包缓存时，沿用 `build` 作业的环境，将同一个起初为空的卷挂到两个独立容器的 `/cache`，对同一提交跑冷、热构建。热跑须有实际命中。比较耗时和包 SHA-256，再用 `test` 作业的安装阶段验证安装与 CLI，最后在线上重跑打包确认命中。
