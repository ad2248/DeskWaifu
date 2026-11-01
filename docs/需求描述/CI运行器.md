# DeskWaifu CI 运行器

> 基线：`0058b7e`。本文区分规范要求、现行设计与证据边界：约束性措辞保留原文要求；现行行为和限制不自动升级为规范；仓库配置及测试描述不代表线上部署已验证。本次迁移不执行运维操作。

## 运维目标（未在线上验证）

本文记录的部署使用 `st` 上的用户服务 `deskwaifu-ci-runner.service`，通过无根 Podman 运行容器任务，配置位于 `~/.config/deskwaifu-ci-runner/config.yaml`。预期限制为：

```yaml
runner:
  capacity: 4
container:
  options: --cpus=8 --memory=8g --memory-swap=8g
```

这些只是片段，不能替换完整配置。保留已注册的运行器身份、通过镜像 ID 固定的 `deskwaifu-arch` 标签、`force_pull: false`、`docker_host: "-"`、`privileged: false` 和 `valid_volumes: []`。

仓库不包含已部署的服务单元或完整运行器配置。线上服务、无根执行、限制、标签映射及上述其他设置均未在此验证。

应用这些限制后，每个容器拥有 8 CPU 的时间配额，不是独占的一组物理核心；内存为 8 GiB，且不允许额外交换空间。限制不是资源预留。四个容器合计内存上限为 32 GiB。此前文档记载 `st` 约有 31 GiB RAM，此处未验证；若属实，在计入其他服务前这些限制就已超配 RAM。并发重型构建可能遇到内存压力或 OOM；监测峰值用量，仅在运维人员批准后调整。

## 仓库可证实的行为

[`.gitea/workflows/ci.yml`](../../.gitea/workflows/ci.yml) 选择 `deskwaifu-arch`，并在注释中要求使用镜像 ID 固定的镜像及 `force_pull: false`；它不配置运行器。主构建、渲染目标变体构建及 sanitizer 构建显式使用 `cmake --build ... --parallel 16`。不要从 `nproc` 推导编译并行度：此任务数有意超过预期的 8 CPU 配额。

[`scripts/rebuild-ci-image.sh`](../../scripts/rebuild-ci-image.sh) 从 `Containerfile` 构建 `ci` 目标，检查镜像并私下传输到 `${DESKWAIFU_RUNNER_HOST:-st}`。镜像包含专有依赖，必须保持私有。脚本检查目标主机存在该镜像 ID，并打印 `deskwaifu-arch:docker://sha256:...` 标签；它不更新运行器配置或重启服务。更新运行器时，手动应用该标签并设置 `force_pull: false`。

## 隔离与线上验证

任务必须保持独立工作区、进程命名空间及运行时目录。不得添加共享可写构建目录挂载、主机网络、主机 PID 命名空间、固定主机端口映射或主机桌面/音频套接字。仅在满足所需隔离时，容器内相同套接字路径才安全。

CI 隔离检查只验证 `/var/run/docker.sock` 不是套接字。[`scripts/test-environment.py`](../../scripts/test-environment.py) 为其启动的测试创建私有运行时目录和 D-Bus 会话；它不是网络/文件系统安全沙箱。这些检查不证明容器工作区、挂载、网络或 PID 隔离。必须针对线上运行器及容器验证这些属性。

更改线上运行器之前，备份配置并等待运行中的任务结束。评审工作流选择不同的 `opencode-review` 标签；这不证明其部署服务独立。重启前确认服务边界，仅重启 DeskWaifu CI 运行器，不重启评审运行器。应用更改后，检查服务健康、实际镜像 ID 和隔离设置，以及新建容器的 CPU 和内存 cgroup 限制。

## 需求验收与证据边界

- 仓库侧核对标签、固定镜像要求、显式构建并行度和镜像脚本职责；不得把镜像传输成功等同于运行器配置已更新。
- 运维验收须另行检查线上服务边界、无根执行、实际隔离及新容器 cgroup 限制，并遵循备份、等待任务完成和批准流程。
- 上述部署状态、主机内存及隔离在本文中仍属未线上验证，不因文档迁移而变为已验证事实。
