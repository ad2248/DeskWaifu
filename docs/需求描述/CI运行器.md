# DeskWaifu CI 运行器

> 基线：`0058b7e`。本文区分规范要求、现行设计与证据边界：约束性措辞保留原文要求；现行行为和限制不自动升级为规范；仓库配置及测试描述不代表线上部署已验证。本次迁移不执行运维操作。

## 运维目标（未在线上验证）

本文记录的部署使用 `st` 上的用户服务 `deskwaifu-ci-runner.service`，通过无根 Podman 运行容器任务，配置位于 `~/.config/deskwaifu-ci-runner/config.yaml`。预期限制为：

```yaml
runner:
  capacity: 4
container:
  options: --cpus=8 --memory=8g --memory-swap=8g --tmpfs=/tmp:rw,exec,nosuid,size=4g,mode=1777 -v /srv/cache/deskwaifu-ci:/cache
```

这些只是片段，不能替换完整配置。保留已注册的运行器身份、通过镜像 ID 固定的 `deskwaifu-arch` 标签、`force_pull: false`、`docker_host: "-"`、`privileged: false` 和 `valid_volumes: []`。其中 `/srv/cache/deskwaifu-ci:/cache` 是唯一的宿主机持久化挂载，仅供编译器缓存（ccache）使用；它由运行器配置统一施加，工作流无法通过 `valid_volumes` 自行挂载其他宿主路径。

仓库不包含已部署的服务单元或完整运行器配置。线上服务、无根执行、限制、标签映射及上述其他设置均未在此验证。

应用这些限制后，每个容器拥有 8 CPU 的时间配额，不是独占的一组物理核心；内存为 8 GiB，且不允许额外交换空间。限制不是资源预留。四个容器合计内存上限为 32 GiB。此前文档记载 `st` 约有 31 GiB RAM，此处未验证；若属实，在计入其他服务前这些限制就已超配 RAM。并发重型构建可能遇到内存压力或 OOM；监测峰值用量，仅在运维人员批准后调整。

## 编译缓存卷（现行设计）

宿主目录 `/srv/cache/deskwaifu-ci` 以 `-v /srv/cache/deskwaifu-ci:/cache` 挂入每个 CI 容器，容器内按消费者分两套 ccache 存储：`/cache/ccache`（普通 CI 任务，root 写入）与 `/cache/ccache-makepkg`（四包发布构建，`dwrel` 构建用户写入）。ccache 以预处理输出与编译器指纹为键、并发安全，同输入同输出（位级一致），不改变测试语义或包字节可复现性。每个存储 `CCACHE_MAXSIZE=8G`，超限自动 LRU 清理；运维可在运行器空闲时整体清空该宿主目录。缓存卷缺失或 `workflow_dispatch` 选择干净构建时，所有入口自动回退到无缓存构建（`CCACHE_DISABLE=1` 或不下发 launcher），不回写缓存。

## 仓库可证实的行为

[`.gitea/workflows/ci.yml`](../../.gitea/workflows/ci.yml) 选择 `deskwaifu-arch`，并在注释中要求使用镜像 ID 固定的镜像及 `force_pull: false`；它不配置运行器。主构建、渲染目标变体、sanitizer 和故障植入构建显式使用 24 路，包构建设置 `CMAKE_BUILD_PARALLEL_LEVEL=24`。不要从 `nproc` 推导编译并行度：此任务数有意超过预期的 8 CPU 配额。测试目录的 GCC Release 编译使用较积极的 GCC 垃圾回收参数，降低多个编译器的合计内存；大型 `main-window-transform-test` 进一步收紧回收并关闭宏展开诊断跟踪（编译错误仍报告，但不展开宏来源链）。这不降低优化级别、不改变生产目标编译参数或全局并发。

CI 编译通过 `CMAKE_<LANG>_COMPILER_LAUNCHER=ccache` 复用宿主机持久缓存；`check_dependencies.py` 所有权检查仍包在缓存外侧，每次编译都执行。四包构建由 `scripts/release-build.sh` 在 `/etc/makepkg.conf.d` 下发缓存配置，仅在挂载存在且未选择干净构建时启用。镜像预装 `ccache`，工作流不得在任务内临时安装软件包。`workflow_dispatch` 的 `clean` 输入（布尔，默认关闭）请求干净构建：本次任务不使用也不回写缓存，不清空共享缓存（避免影响并发任务）。

[`scripts/rebuild-ci-image.sh`](../../scripts/rebuild-ci-image.sh) 从 `Containerfile` 构建 `ci` 目标，检查镜像并私下传输到 `${DESKWAIFU_RUNNER_HOST:-st}`。镜像包含专有依赖，必须保持私有。脚本检查目标主机存在该镜像 ID，并打印 `deskwaifu-arch:docker://sha256:...` 标签；它不更新运行器配置或重启服务。更新运行器时，手动应用该标签并设置 `force_pull: false`。

## 隔离与线上验证

任务必须保持独立工作区、进程命名空间及运行时目录。不得添加共享可写构建目录挂载、主机网络、主机 PID 命名空间、固定主机端口映射或主机桌面/音频套接字。唯一的例外是上述运行器托管的编译缓存卷：它只存放编译器缓存（无源码、无工作区数据、无凭据），不进入测试运行时环境，也不削弱逐项 HOME/XDG/D-Bus 隔离；其余共享挂载仍然禁止。仅在满足所需隔离时，容器内相同套接字路径才安全。

CI 隔离检查只验证 `/var/run/docker.sock` 不是套接字。[`scripts/test-environment.py`](../../scripts/test-environment.py) 为其启动的测试创建私有运行时目录和 D-Bus 会话；它不是网络/文件系统安全沙箱。这些检查不证明容器工作区、挂载、网络或 PID 隔离。必须针对线上运行器及容器验证这些属性。

更改线上运行器之前，备份配置并等待运行中的任务结束。评审工作流选择不同的 `opencode-review` 标签；这不证明其部署服务独立。重启前确认服务边界，仅重启 DeskWaifu CI 运行器，不重启评审运行器。应用更改后，检查服务健康、实际镜像 ID 和隔离设置，以及新建容器的 CPU 和内存 cgroup 限制。

## 需求验收与证据边界

- 仓库侧核对标签、固定镜像要求、显式构建并行度和镜像脚本职责；不得把镜像传输成功等同于运行器配置已更新。
- 运维验收须另行检查线上服务边界、无根执行、实际隔离及新容器 cgroup 限制，并遵循备份、等待任务完成和批准流程。
- 上述部署状态、主机内存及隔离在本文中仍属未线上验证，不因文档迁移而变为已验证事实。
