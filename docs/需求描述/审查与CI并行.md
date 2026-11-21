# PR 代码审查与 CI 并行执行

PR 创建、重新打开或推送新提交时独立触发 CI 和自动审查。CI 也响应 `main` 的 push 和手动运行。审查使用 `pull_request_target` 和 `opencode-review` runner，从事件中的 `pull_request.base.sha` 检出目标分支工作流与脚本。 [Rust CI](<../../.gitea/workflows/ci.yml>) · [OpenCode 审查工作流](<../../.gitea/workflows/opencode-review.yml>)

审查脚本读取 PR 上下文并生成 merge-base 到待审查提交的 diff。临时目录只用于存放审查输入，执行权限由 runner 和 OpenCode 配置决定。审查无需等待 CI，实际并发取决于 runner 可用槽位。审查只报告实际进行的独立验证。 [审查入口](<../../.gitea/scripts/opencode-review.sh>)

## 验收依据

PR 正文首行可填写 `验收依据: base:计划/某方案.md`、`验收依据: head:计划/某方案.md` 或 `验收依据: #123`。`base:` 从事件指定的目标分支提交读取文件，`head:` 从待审查提交读取文件，Issue 引用读取本仓库的标题和正文。省略字段时使用 PR 描述。正文应说明变更范围、验收项和实际验证。 [验收依据解析](<../../.gitea/scripts/resolve-review-acceptance.py>)

显式引用无法读取或解析时发布 `COMMENT`。已有该 head SHA 的自动审查会跳过重复评审。验收范围变更或引用修复后，随新提交重新送审。 [审查入口](<../../.gitea/scripts/opencode-review.sh>)

## 合并保护与验收

Gitea 的 `main` 分支保护运维要求如下。状态检查名称和审批资格以线上 PR 及仓库设置为准。

- 要求当前提交的必需 CI 检查通过。根据实际状态上下文配置检查项。
- 要求至少一名具备 `Reviewer` 资格的审批者批准，并启用 **Ignore stale approvals**。
- 要求管理员遵守分支保护，并禁止通过绕过名单、直接推送或强制推送跳过规则。

审查工作流只有收到明确批准信号才发布 `APPROVED`，否则发布 `COMMENT`。脚本在登记建议、批准和发布前分别复核 head，发布的 Review 绑定待审查提交。复核与操作之间存在竞态，已经创建的 issue 会保留。 [审查动作](<../../.gitea/scripts/opencode-review-action.sh>) · [审查入口](<../../.gitea/scripts/opencode-review.sh>)

```sh
python3 -B -m unittest discover -s .gitea/scripts -p 'test_*.py' -v
```

本地测试使用模拟 CLI 和本地 Git 仓库。线上并行触发、当前提交的检查与有效批准，以及推送新提交之后的合并条件，需在目标分支已包含相应工作流时以测试 PR 核验。 [审查测试](<../../.gitea/scripts/test_parallel_review.py>)
