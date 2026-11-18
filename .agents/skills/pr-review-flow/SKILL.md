---
name: pr-review-flow
description: Gitea/Forgejo 端到端 PR 评审工作流。给定 PR URL，审查 diff、核验当前提交的 CI、按需本地测试，通过评审评论批准或驳回，并将非阻塞发现登记为 issue。Trigger when the user provides a pull request URL and asks to review/approve/reject it.
user-invokable: true
---

# PR 评审流程

完成一个 PR 的完整评审工作流。输入：形如
`https://<host>/<owner>/<repo>/pulls/<n>` 的 PR URL。

选择机器上可用的 `gt` 或 `tea`，先查看所安装版本的 `--help`，再配合
本地 git 完成以下目标；不要假定不同工具或版本的命令语法一致。
除非用户明确要求，否则不要合并。

## 1. 解析 URL

从 URL 中提取主机、仓库 slug `<owner>/<repo>` 与 PR 编号 `<n>`。
每次远程操作都明确选择目标主机和仓库，并确认使用的账号，避免依赖
当前目录或默认配置误操作其他实例、仓库或账号。

## 2. 获取 PR 元信息

使用所选 CLI 获取 PR 元信息，记录 base 分支、head 分支、head SHA、
状态、冲突情况，以及 PR 正文（摘要 + 声称的测试）。

## 3. 本地检出并阅读 diff

检出前记录原分支（若原本是 detached HEAD，则记录原 SHA），并确认工作区
没有会被覆盖的未提交改动。根据所选 CLI 的帮助检出 PR 提交，不要假定
工具会进入 detached HEAD，也不要假定复用的本地分支等于当前 PR head。
使用 `git rev-parse HEAD` 确认本地 HEAD 等于已记录的 PR head SHA。

使用所选 CLI 获取并阅读 PR diff；需要本地统计或补充对比时可用 git
（把 `<base>` 替换为真实 base 分支，并确认 `origin` 指向目标仓库、
`origin/<base>` 已更新）：

```bash
git diff origin/<base>...HEAD --stat
git diff origin/<base>...HEAD
```

diff 不清晰处要阅读周边代码；不要孤立地评审代码块。

## 4. 核验 CI 并按需本地测试

使用所选 CLI 查询 base 分支保护规则和 PR 当前 head SHA 的 CI 状态；
必要时使用该 CLI 提供的 API 访问能力。

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

测试失败、行为破坏或存在阻塞性缺陷时**驳回 / 请求修改**。
使用所选 CLI 提交相应结论与评审正文，确保评审针对已核验的 head SHA。

## 6. 撰写评审评论

按所选 CLI 的帮助传入评审正文并提交批准或请求修改的结论。
提交评审前重新确认 PR head SHA 未变化；若变化，重新核验 diff 和 CI。
若已有待提交评审，先检查其正文和行内评论，以及工具提交时是否会一并
发布或替换这些内容，不要盲目跳过确认。

以用户的语言撰写评论，结构如下：

- 结论先行（LGTM 或发现问题）
- 亮点（做得好的地方）
- 建议，凡不阻塞批准的建议都要明确标注为非阻塞
- 分别说明已核验的 CI，以及本地实际运行的测试和结果；若因已核验
  当前提交的必需 CI 而省略本地测试，应明确说明，不得报告为本地通过

## 7. 将非阻塞发现登记为 issue

每个发现单独建一个 issue，便于跟踪：

使用所选 CLI 在目标仓库创建 issue，填写标题和结构化正文。

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

使用标签前先通过所选 CLI 检查目标仓库是否定义了这些标签——仓库可能
没有定义任何标签。最终向用户汇报时提及新建 issue 的链接。

## 8. 清理并汇报

回到检出前记录的原分支（`git checkout <original-branch>`；若原本为
 detached HEAD，使用 `git checkout --detach <original-sha>`），然后向用户汇报：
结论、测试结果、评审评论链接以及相关 issue 链接。

## 陷阱

- 不要把 `gt` 与 `tea` 的命令或参数混用；以当前安装版本的 `--help`
  为准
- 不要使用旧 head SHA 的 CI 成功状态，也不要用本地成功覆盖必需 CI 失败
- 评审过程中绝不提交、推送或合并，除非用户要求
