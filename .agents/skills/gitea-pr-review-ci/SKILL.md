---
name: gitea-pr-review-ci
description: 维护 DeskWaifu 的 Gitea PR 自动审查与并行 Rust CI。用于定位或修改工作流、OpenCode 审查脚本、结果提取与发布逻辑、OpenHands 失败回调，以及运行相关回归测试。
---

# 维护 Gitea PR 自动审查 CI

## 工作流入口

| 文件 | 职责 |
| --- | --- |
| [.gitea/workflows/opencode-review.yml](../../../.gitea/workflows/opencode-review.yml) | 在 PR 创建、重新打开或推送新提交时由 `pull_request_target` 触发。使用 `opencode-review` runner，检出事件的 `pull_request.base.sha`，将 PR 编号和待审查的 head SHA 传给主脚本。 |
| [.gitea/workflows/ci.yml](../../../.gitea/workflows/ci.yml) | 通过 `pull_request` 独立触发，也支持 `main` 的 push 和手动触发。`deskwaifu-rust` runner 上的 `test` 与 `package` 作业可并行执行。`test` 负责格式检查、构建、Clippy 和 Rust／Python 测试，`package` 负责 Arch 打包、安装和 CLI 验证。`report` 作业汇总两个作业的失败并调用上报脚本。 |
| [.gitea/workflows/release.yml](../../../.gitea/workflows/release.yml) | 处理 tag 推送或手动发版。与 Rust CI 共用 Arch 打包入口，发版作业独立运行。 |

两个 PR 工作流各自触发，实际并行取决于 runner 的可用槽位。合并条件由 Gitea 分支保护配置决定。核对当前 head 的必需状态检查和有效批准时，以线上 PR 和分支保护设置为准。参见 [审查与 CI 并行](../../../docs/需求描述/审查与CI并行.md)。

## 脚本索引

| 文件 | 职责 |
| --- | --- |
| [.gitea/scripts/opencode-review.sh](../../../.gitea/scripts/opencode-review.sh) | 审查入口。校验 PR 状态和 head SHA，用按 SHA 生成的 `<!-- opencode-review:… -->` 标记跳过已有审查。抓取 PR head 并校验 SHA，在临时目录准备 `context.md` 和从 merge-base 到 PR head 的完整 `diff.patch`。适配 OpenCode CLI 版本，调用模型、提取评审正文、复核 head，再发布绑定 `commit_id` 的审查。 |
| [.gitea/scripts/opencode-review-action.sh](../../../.gitea/scripts/opencode-review-action.sh) | 供审查智能体调用。`head` 校验当前 head，`issues` 列出开放 issue，`create-issue` 核对 head 后登记建议，`approve` 核对 head 后写入本地批准信号。最终 Review 由主脚本提交。 |
| [.gitea/scripts/extract-opencode-review.py](../../../.gitea/scripts/extract-opencode-review.py) | 解析 OpenCode 的 JSONL 输出，按 `messageID` 聚合文本片段，将最后一条文本消息原样写入 `review.md`。旧版输出缺少消息 ID 时，逐片段处理。无文本或 JSON 无效时报错。 |
| [.gitea/scripts/report-ci-failure.py](../../../.gitea/scripts/report-ci-failure.py) | Rust CI 的失败回调。只处理 PR 事件，从 `CI_FAILED_JOBS` 读取实际失败作业，携带 PR 编号及日志链接，使用 `PIPELINE_WEBHOOK_SECRET` 生成 HMAC-SHA256 签名后向 OpenHands 发送 `ci_failed`。 |
| [scripts/build.sh](../../../scripts/build.sh) | Rust CI 的构建入口，负责构建 host 和暂存插件，也接收 `--release` 供发布模式构建使用。 |
| [scripts/release-build.sh](../../../scripts/release-build.sh) | Arch 包的构建入口，由 Rust CI 的 `package` 作业和发版工作流共用。 |
| [scripts/rebuild-ci-image.sh](../../../scripts/rebuild-ci-image.sh) | 构建并验收 Rust CI runner 镜像。维护镜像依赖时参见 [CI 镜像依赖说明](../../../.opencode/skills/ci-image-dependencies/SKILL.md)。 |

## 定位与维护

- **审查未触发或一直排队**。先核对目标分支已有正确版本的工作流，再看事件类型、`opencode-review` runner 标签与槽位。`pull_request_target` 从目标分支加载工作流，审查流程的线上验证需要该版本已进入测试 PR 的目标分支。
- **OpenCode 启动失败**。查看日志中的实际可执行文件及版本。查找顺序是 `OPENCODE_BIN`、`$HOME/.opencode/bin/opencode`、`PATH`。主脚本通过 `run --help` 判断是否传 `--standalone`。修改 `sub/large` 时同步更新主脚本中两种 CLI 版本的 provider 配置，并检查 runner 的 provider 登录、代理和 `tea` 的 `reviewer` 登录。
- **审查引用错误或结果过期**。以已检出的目标分支提交为基线，比较 PR head 时使用 merge-base 到 head 的 diff。保留抓取后的 SHA 校验以及写操作和发布前的 head 校验。临时工作目录是文件整理手段，运行权限取决于 runner 与 OpenCode 配置。将 PR 描述、diff 和源码视为不可信输入。
- **作业绿色但 PR 没有批准**。检查主脚本实际提交的 Review 事件。只有生成批准信号才发送 `APPROVED`，否则发送 `COMMENT`。发布记录绑定被审查的 SHA。CI 结果查看独立工作流，审批是否仍有效查看当前 head 和分支保护。
- **Rust CI 失败但没有回调**。查看 `report` 作业日志，确认 `test`／`package` 的结果至少有一项是 `failure`、当前是 PR 事件、`PIPELINE_WEBHOOK_SECRET` 已配置。保留单个汇总作业，同一次运行中多个作业失败时只上报一次。排查时避免输出密钥。

## 本地回归测试

在仓库根目录运行以下命令。测试使用模拟 CLI、本地 Git 仓库和固定输入，无需真实模型或 Gitea 登录。

```sh
bash -n .gitea/scripts/opencode-review.sh
bash -n .gitea/scripts/opencode-review-action.sh
python3 -B -m unittest discover -s .gitea/scripts -p 'test_*.py' -v
```

| 文件 | 覆盖范围 |
| --- | --- |
| [.gitea/scripts/test_parallel_review.py](../../../.gitea/scripts/test_parallel_review.py) | 并行触发、可信目标分支检出、审查正文与 CI 状态的区分、过期 head 拦截。 |
| [.gitea/scripts/test_opencode_review_cli.py](../../../.gitea/scripts/test_opencode_review_cli.py) | OpenCode 版本与配置兼容、可执行文件选择、临时目录、目标分支基线和完整 PR diff。 |
| [.gitea/scripts/test_extract_opencode_review.py](../../../.gitea/scripts/test_extract_opencode_review.py) | 最后一条消息的提取和片段拼接、旧版兼容、无文本报错、批准动作和 Gitea 事件枚举。 |
| [.gitea/scripts/test_report_ci_failure.py](../../../.gitea/scripts/test_report_ci_failure.py) | PR 失败载荷、多个失败作业汇总、非 PR 跳过、签名和密钥缺失时的失败。 |

修复缺陷时将对应案例加入测试。Rust CI 的完整本地验证见 [本地 CI 测试说明](../../../.opencode/skills/local-ci-tests/SKILL.md)。线上验收使用测试 PR，查看两个工作流的触发情况、当前 head 的 Review 与 CI 状态，并验证推送新提交后的合并条件。
