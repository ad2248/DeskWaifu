---
name: issue-task-flow
description: 根据 Gitea/Forgejo issue 完成任务并闭环交付。Use when the user provides an issue URL or number and asks to implement, fix, investigate, or otherwise handle that issue through verification, issue reply, commit, push, and a PR to main when repository changes are made.
user-invokable: true
---

# Issue 任务闭环

端到端完成一个 issue。Gitea/Forgejo 操作使用 `tea`，本地改动使用
`git`。若仓库存在 `AGENTS.md`、`tea.md` 和
`.opencode/skills/submit-code-and-pr/SKILL.md`，先阅读它们。

## 完成契约

在 issue 收到描述结果的最终回复之前，任务不算完成。

- 开始实现前：专用工作分支必须已创建或切换并推送，issue 必须收到
  精确文本「工作开始」的评论，且其关联分支必须指向该工作分支。
- 仓库文件有改动时：验证改动、提交、推送分支、创建指向 `main`
  的 PR，然后在 issue 中回复 PR URL。
- 仓库文件无改动时：在 issue 中回复调查过程、结论与验证证据；
  不要创建空提交或空 PR。
- 遇到阻塞时：不得声称已完成。回复阻塞原因、证据以及所需的
  具体决策或输入，除非用户要求暂不发布。

不要手动关闭 issue。改动完成时在 PR 描述中写入 `Closes #N`，
让合并 PR 自动关闭它。

## 1. 识别 issue

接受 issue URL 或无歧义的 issue 编号。从 URL 中提取仓库 slug 与
issue 编号；只有裸编号时，根据当前 Git 远端确定仓库。

若 issue 或仓库存在歧义，先提一个简短的问题，不要猜测。每条会
改动状态的 `tea` 命令都要显式传入 `--login` 与 `--repo`。

```bash
tea issues <issue> --login <login> --repo <owner/repo> --comments \
  --fields index,state,title,body,labels,comments,url
```

阅读完整正文与所有评论。记录：

- 期望行为与验收标准
- 复现细节与受影响的代码路径
- 后续评论中的决策或更正
- 依赖、相关 issue 与明确的非目标

不要仅凭标题就开始实现。若检查仓库后仍有实质性需求不明确，
先列出可能的解读并提问，再动手编码。

## 2. 准备工作区

编辑前先检查当前状态：

```bash
git status --short --branch
git remote -v
git log --oneline -10
git fetch origin
```

- 保留无关的用户或 agent 改动；未经同意绝不重置、丢弃或夹带它们。
- 不要直接在 `main` 上工作。使用用户明确指定的分支，或基于最新
  `origin/main` 创建专用分支（如 `fix/issue-<N>-<topic>`）。
- 若切换分支会危及未提交的工作或与其他 worktree 冲突，先停下来
  询问，不要使用破坏性命令。
- 确认最终相对 `origin/main` 的 diff 只包含本 issue 的工作。

## 3. 登记任务开始

完成工作区准备后、开始实现前，严格按以下顺序登记任务状态：

1. 确认当前分支是本 issue 的专用工作分支，不得是 `main` 或仓库
   默认分支；将分支推送到远端，使 Forgejo/Gitea 能解析该分支。
2. 在 issue 中发布内容精确为「工作开始」的评论，不添加标点、分支名
   或其他文字。
3. 通过 issue 的 `ref` 字段把当前工作分支关联到 issue。`tea` 没有
   对应子命令时，使用 `tea api`，不得用评论中的分支名代替关联。
4. 分别复查评论和 issue 的 `ref`；两者都正确后才能开始实现。

所有修改 Forgejo/Gitea 状态的 `tea` 命令都必须显式传入 `--login`
和 `--repo`：

```bash
branch=$(git branch --show-current)
default_branch=$(git symbolic-ref --short refs/remotes/origin/HEAD 2>/dev/null)
default_branch=${default_branch#origin/}
test -n "$default_branch" || default_branch=main
test -n "$branch"
test "$branch" != main
test "$branch" != "$default_branch"
git push -u origin HEAD

tea comment <issue> "工作开始" \
  --login <login> --repo <owner/repo>
tea api /repos/<owner>/<repo>/issues/<issue> -X PATCH \
  -f ref="refs/heads/$branch" \
  --login <login> --repo <owner/repo>

tea issues <issue> --login <login> --repo <owner/repo> --comments \
  --fields index,state,title,comments,url
tea api /repos/<owner>/<repo>/issues/<issue> \
  --login <login> --repo <owner/repo>
```

复查时确认评论中存在独立的精确文本「工作开始」，且 API 响应中的
`ref` 等于 `refs/heads/<branch>`。如果启动评论、分支关联或任一复查
失败，立即停止，不得继续实现；向用户报告已完成的步骤、失败命令、
错误信息和解除阻塞所需的操作。

## 4. 定义并实现修复

编辑前先把 issue 转化为简短、可验证的清单。缺陷类问题先复现，
或尽可能先添加一个失败的回归测试。功能类需求先找出能证明每条
验收标准的最小测试。

然后：

1. 阅读相关实现与测试，包括周边代码。
2. 做满足 issue 的最小改动；避免无关清理。
3. 仅当行为变化需要时才更新协议或架构文档。
4. 为变更的行为添加或更新测试。

不要悄悄扩大范围。无关缺陷记录到最终报告中，而不是在同一个
PR 里顺手修掉。

## 5. 验证结果

先运行最窄相关的检查，可行时再运行仓库更完整的测试或构建
命令。将检查最终 diff 作为验证的一部分：

```bash
git diff --check
git diff --stat
git diff
git status --short
```

绝不能把未执行的测试报告为通过。若测试无法运行，说明确切的
命令、失败原因或环境限制以及残余风险。

## 6. 交付仓库改动

若被跟踪文件有改动，遵循
`.opencode/skills/submit-code-and-pr/SKILL.md`。重点包括：

1. 只暂存属于本 issue 的文件。
2. 写简洁的「约定式提交」；不要 amend 或跳过钩子。
3. 推送当前分支，不要强推。
4. 创建 PR：当前分支作为 `head`，`main` 作为 `base`。
5. 正文包含摘要、测试命令与结果，以及 `Closes #N`。
6. 检查 `git diff origin/main...HEAD`，然后确认 PR 已开启且
   head/base 分支符合预期。

```bash
git add <issue-files>
git commit -m "fix: concise issue summary"
git push -u origin HEAD

tea pulls create --login <login> --repo <owner/repo> \
  --head <branch> --base main \
  --title "fix: concise issue summary" \
  --description $'## Summary\n\n- change\n\n## Tests\n\n- command: passed\n\nCloses #N'
```

若无被跟踪文件改动，跳过提交、推送与 PR 创建。绝不要为了表示
「调查过该 issue」而创建空提交。

## 7. 回复 issue

在 PR 创建后，或得出经过验证的「无需改动」结论后，发布一条
最终的 issue 评论。使用真实换行，不要发送字面 `\\n` 文本。

改动完成时，包含：

- 实现了什么，以及重要的工程决策
- 运行的测试及其结果
- PR URL
- 已知限制或后续事项

```bash
tea comment <issue> $'已完成：<summary>。\n\n验证：<commands and results>。\n\nPR：<url>' \
  --login <login> --repo <owner/repo>
```

「无需改动」结论或阻塞情形下，用证据与下一步行动替换 PR 一行。
没有交付修复时，不要声称 issue 已修复。

## 8. 最终检查

- 可行时复查 PR 元信息与 issue 评论。
- 运行 `git status --short --branch`；代码交付后，分支必须干净
  并与上游同步。
- 向用户报告 issue URL、测试结果、commit hash 与 PR URL。仅当
  仓库无改动时才省略 commit 与 PR，并说明原因。
