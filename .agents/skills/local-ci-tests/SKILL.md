# 本地执行 DeskWaifu Rust CI

- 权威步骤为 `.gitea/workflows/ci.yml`。分别验证 test 与 package job，不把本地检查描述为远端 CI 已通过。
- 从 `st:~/.config/deskwaifu-ci-runner/config.yaml` 读取 `deskwaifu-rust:docker://sha256:...`，确认本地镜像 ID 存在，不使用漂移的 latest。
- 先检查工作区，不覆盖用户改动。以 rootless Podman 运行固定镜像，按 workflow 执行 cargo fmt、clippy、test、release build 和 Python 工具测试。
- Cargo 使用 `--frozen`，不在 workflow 临时安装工具或忽略失败。
- package job 由容器内 root 编排，`scripts/release-build.sh` 创建非 root makepkg 用户。源码与可信发布工具必须对该用户不可写，且不能被通过祖先目录替换。
- 对未提交代码进行打包验证时，可在 `/tmp/opencode` 创建独立 Git 快照，仅提交到该临时仓库，不擅自提交工作分支；将其以只读方式挂载进容器，构建目录使用可写 tmpfs。
- 运行结束检查源码工作区状态。报告镜像 ID、具体步骤、通过/失败/跳过及未验证项，不自动提交、推送、发布或改 runner。
