---
name: pr-review-flow
description: Gitea/Forgejo 端到端 PR 评审工作流。给定 PR URL，审查 diff、核验当前提交的 CI、按需本地测试，通过评审评论批准或驳回，并将非阻塞发现登记为 issue。Trigger when the user provides a pull request URL and asks to review/approve/reject it.
user-invokable: true
---

# PR 评审流程

完成一个 PR 的完整评审工作流。输入：形如
`https://<host>/<owner>/<repo>/pulls/<n>` 的 PR URL。

优先使用 `tea` CLI 加本地 git；除非用户明确要求，否则不要合并。

## 1. 解析 URL

从 URL 中提取仓库 slug `<owner>/<repo>` 与 PR 编号 `<n>`。每次调用 `tea`
都显式传入 `--repo <owner>/<repo>`。

## 2. 获取 PR 元信息

```bash
tea pulls <n> --repo <owner>/<repo>
```

记录 base 分支、head 分支、head SHA、状态、冲突情况，以及 PR 正文
（摘要 + 声称的测试）。

## 3. 本地检出并阅读 diff

```bash
tea pulls checkout <n>
```

这会停留在 PR 跟踪分支的 detached HEAD 上。对大 diff 而言 `tea pulls
<n> --fields diff` 并不可靠——请改用 git 阅读 diff
（把 `<base>` 替换为真实 base 分支）：

```bash
git diff origin/<base>...HEAD --stat
git diff origin/<base>...HEAD
```

diff 不清晰处要阅读周边代码；不要孤立地评审代码块。

## 4. 核验 CI 并按需本地测试

先核验 base 分支保护规则和 PR 当前 head SHA 的 CI 状态：

```bash
tea api repos/<owner>/<repo>/branch_protections/<base> \
  --repo <owner>/<repo>
tea api repos/<owner>/<repo>/commits/<head-sha>/status \
  --repo <owner>/<repo>
```

只有状态属于 PR 当前 head SHA、所有必需检查均成功，且 CI 覆盖相关改动
时，才可直接信任 CI 并省略本地完整测试。旧提交的成功状态无效；base
分支未启用必需状态检查时，也不能用任意可选 CI 代替本地验证。

以下情形必须查找项目测试命令（README、AGENTS.md、scripts/）并在本地
执行相关测试：

- PR 修改 CI 或测试基础设施，例如工作流、Containerfile、测试入口脚本
- CI 未运行、不可用，或没有覆盖被修改的行为
- 发现疑似 flaky、环境相关行为，或需要针对评审发现执行聚焦测试

必需 CI 失败时不得批准；本地测试可用于诊断，但本地成功不能覆盖失败
的必需检查。需要运行的容器/CI 风格测试套件经常超过 120 s，应调大
timeout（例如 `300000` ms），而不是让它中途被中止。

## 5. 决定：批准或驳回

当前 head SHA 的全部必需检查通过、所有必要的本地测试通过，且没有
阻塞性缺陷（崩溃、安全问题、行为破坏、缺少文档与测试更新的协议
变更）时**批准**。

测试失败、行为破坏或存在阻塞性缺陷时**驳回 / 请求修改**：

```bash
tea pulls reject <n> --repo <owner>/<repo> '<comment>'
```

## 6. 撰写评审评论

评论是位置参数——没有 `--message` 旗标：

```bash
tea pulls approve <n> --repo <owner>/<repo> '<comment>'
```

以用户的语言撰写评论，结构如下：

- 结论先行（LGTM 或发现问题）
- 亮点（做得好的地方）
- 建议，凡不阻塞批准的建议都要明确标注为非阻塞
- 分别说明已核验的 CI，以及本地实际运行的测试和结果；若因已核验
  当前提交的必需 CI 而省略本地测试，应明确说明，不得报告为本地通过

## 7. 将非阻塞发现登记为 issue

每个发现单独建一个 issue，便于跟踪：

```bash
tea issues create --repo <owner>/<repo> \
  --title '<component>: short problem statement' \
  --description '<structured body>'
```

Issue 正文模板：

```markdown
来源: PR #<n> 审查意见 (非阻塞)

## 问题
<what and where — quote the code, name the file/function>

## 影响
<consequences; state honestly if currently minor>

## 建议
<concrete fix options>

## 参考
<related code paths / reproduction>
```

使用标签前先执行 `tea labels`——仓库可能没有定义任何标签。最终向
用户汇报时提及新建 issue 的链接。

## 8. 清理并汇报

回到原分支（`git checkout <base>`），然后向用户汇报：
结论、测试结果、评审评论链接以及相关 issue 链接。

## 陷阱

- `tea pulls approve`/`reject` 的评论是位置参数，不能通过旗标传入
- `tea pulls checkout` 会停留在 detached HEAD；请对 `origin/<base>` 做 diff
- 不要使用旧 head SHA 的 CI 成功状态，也不要用本地成功覆盖必需 CI 失败
- 评审过程中绝不提交、推送或合并，除非用户要求
