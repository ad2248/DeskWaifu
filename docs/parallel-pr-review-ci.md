# PR 代码审查与 CI 并行执行

PR 创建、重新打开或推送新提交后，两个工作流独立触发：

- `.gitea/workflows/ci.yml` 使用 `pull_request`，负责构建和自动化测试。
- `.gitea/workflows/opencode-review.yml` 使用 `pull_request_target`，从可信基线加载工作流，并检出 `main` 上的审查脚本。只读取 PR 上下文和 diff，不检出或执行 PR 代码，不构建、不运行自动化测试。

审查智能体仍检查测试覆盖缺口，可以建议补充测试；不会等待 CI，也不会声称 CI 已通过。两个 runner 必须分别有可用执行槽位，才能实际同时运行。

## 合并保护

工作流文件本身不会配置分支保护。Gitea 仓库设置 → 分支 → `main` 需要同时满足：

- 开启状态检查，要求精确上下文：`DeskWaifu CI / Arch build and headless Wayland test (pull_request)`。
- 至少一次批准，且有效审批者仅限 `Reviewer`。
- 开启 **Ignore stale approvals**：保留历史审查，但旧提交的批准不计入当前提交的合并条件。
- 开启“管理员必须遵守分支保护”，不允许通过绕过名单跳过保护。
- 保持禁止直接推送和强制推送。

审查脚本正常结束只表示结果已处理，不表示通过审查：无批准信号时发布 `COMMENT`，只有明确批准才发布 `APPROVED`。因此不能用审查工作流的绿色状态代替正式批准。

## 验收

- CI 失败、排队、运行中或缺失时，即使审查批准，也不能合并。
- CI 成功但 `Reviewer` 尚未批准当前提交时，不能合并。
- CI 成功且 `Reviewer` 批准当前提交后，才满足这两项合并条件；其他分支保护规则仍然适用。
- 推送新提交后必须等待新提交的 CI 和批准；审查期间 head 变化时丢弃旧审查结果。

本地回归测试（不连接模型、不发布审查）：

```sh
python3 -B -m unittest discover -s .gitea/scripts -p 'test_*.py' -v
```

并行触发的线上验收需在工作流进入 `main` 后，用测试 PR 检查上述场景。`pull_request_target` 会读取基线工作流，仅修改 PR 分支不会激活新的触发方式。
