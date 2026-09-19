---
name: submit-code-and-pr
description: 提交代码、回复工单并创建 PR。Use when the user asks to commit changes, push a branch, submit a Gitea/Forgejo pull request, and update the related issue.
---

# 提交代码与 PR

本地改动使用 `git`，Gitea/Forgejo 操作使用 `tea`。若仓库存在
`tea.md`，先阅读它。

## 关联工单判定

整个流程只「确认已知关联工单」，不「搜索潜在关联工单」：

- 仅当用户明确提供工单编号，或当前上下文已明确建立关联（例如
  任务本身来自某个 issue 的闭环流程）时，才将该工单视为相关
  工单。
- 用户未指定关联工单时，不要主动搜索开放工单或按改动内容推断
  潜在关联；此时省略 `Closes #N`，也跳过工单回复步骤。
- 关联关系存在歧义（编号未给出但疑似相关、存在多个候选等）时，
  先询问用户，不得自行猜测。
- 对已明确关联的工单，只核对其编号存在且语义相符；发现明显不
  符时向用户报告，不要改挂其他工单。

## 工作流程

1. 提交前检查：
   - 运行 `git status --short --branch`、`git diff --check`、`git diff`
     和 `git log --oneline -10`。
   - 确认当前分支、远端、base 分支、issue 要求与测试结果。
   - 按「关联工单判定」确认是否存在已明确关联的工单；没有则后
     续步骤一律省略工单相关操作，也不要为此搜索开放工单。
   - 未经用户明确同意，不得夹带无关改动。
2. 验证改动：
   - 先运行最小相关的测试，可行时再运行仓库的完整测试命令。
   - 无法运行的测试要如实说明；绝不能把未执行的测试报告为通过。
3. 提交并推送：
   - 只暂存已获认可的文件。
   - 使用符合仓库历史的简洁「约定式提交」信息。
   - 除非用户明确要求，不得 amend、跳过钩子、强推或更改 Git 配置。
   - 分支没有上游时，用 `git push -u origin HEAD` 推送。
4. 用 `tea pulls create` 创建 PR：
   - 已知时显式传入 `--login` 和 `--repo`。
   - `--head` 设为当前分支，`--base` 设为仓库默认分支，除非用户
     另行指定 base。
   - 正文包含简短的改动摘要与验证结果；仅当存在已明确关联的工
     单且合并后应关闭它时，才包含 `Closes #N`。
5. 仅在存在已明确关联的工单、且 PR 创建之后，才回复该 issue：
   - 使用 `tea comment N "..."`，包含 PR URL、完成的工作、测试
     结果以及明确的工程结论。
   - 评论需要真实换行时，在 Bash 中使用 `$'line one\n\nline two'`；
     不要发送字面 `\\n` 文本。
   - PR 已包含 `Closes #N` 时，不要手动关闭 issue。
6. 核验交付：
   - 确认 PR 处于开启状态，且 head/base 分支符合预期。
   - 可行时复查 issue 评论，并运行 `git status` 确认分支干净且
     已与上游同步。
   - 向用户返回 commit hash 与 PR URL。

## 命令模板

```bash
git status --short --branch
git diff --check
git diff
git log --oneline -10

git add <approved-files>
git commit -m "fix: concise description"
git push -u origin HEAD

# 存在已明确关联的工单时才用下面的 description（含 Closes #N）；
# 没有时省略 Closes #N，并跳过末尾的 tea comment。
tea pulls create --login <login> --repo <owner/repo> \
  --head <branch> --base <base> \
  --title "fix: concise description" \
  --description $'## Summary\n\n- change\n\n## Tests\n\n- command\n\nCloses #N'

# 仅对已明确关联的工单执行：
tea comment N $'Implemented in PR #P.\n\nTests: all passed.' \
  --login <login> --repo <owner/repo>
```
