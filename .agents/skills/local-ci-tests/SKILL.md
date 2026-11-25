# 本地执行 DeskWaifu Rust CI

- 权威步骤为 `.gitea/workflows/ci.yml`。分别验证 `build` 与 `test` 作业，不把本地检查描述为远端 CI 已通过。
- 从 `st:~/.config/deskwaifu-ci-runner/config.yaml` 与 `st:~/.config/deskwaifu-gui-runner/config.yaml` 读取各自的固定镜像 ID，确认本地镜像存在，不使用漂移的 latest。
- 先检查工作区，不覆盖用户改动。以 rootless Podman 运行固定镜像：`build` 用 rust 镜像跑 `scripts/release-build.sh`；`test` 用 gui 镜像映射 `/dev/dri/renderD128` 与 `/transfer`，执行 `tests/acceptance/run_suite.py`。
- 包经宿主 `/srv/cache/deskwaifu-ci/transfer` 传递：`build` 写 `<run>-<sha>/` 与 `ready.json`，`test` 用 `verify_transfer.py` 校验后安装。本地复现时可以用 `run_suite.py --bin-dir` 跳过安装。
- Cargo 使用 `--frozen`，不在 workflow 临时安装工具或忽略失败。
- `build` 由容器内 root 编排，`scripts/release-build.sh` 以固定的 `builder` 账户运行 makepkg；源码归档、`CARGO_HOME` 与 `PKGDEST` 都放在该账户可写的临时目录里。
- 对未提交代码进行打包验证时，可在 `/tmp/opencode` 创建独立 Git 快照，仅提交到该临时仓库，不擅自提交工作分支；将其以只读方式挂载进容器，构建目录使用可写 tmpfs。
- 运行结束检查源码工作区状态。报告镜像 ID、具体步骤、通过/失败/跳过及未验证项，不自动提交、推送、发布或改 runner。
