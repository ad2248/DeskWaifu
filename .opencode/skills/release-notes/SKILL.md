---
name: release-notes
description: 生成从上一次 release 到最新 main HEAD 的中文版本更新说明，用于填写新版本的功能更新。Use when the user asks for release notes, a changelog, or changes since the latest x.y.z release tag.
---

# 生成版本更新说明

## 目标与边界

- 默认输出中文 Markdown，可直接粘贴到新 release 的更新说明中。
- 比较范围固定为上一次 release tag 到最新 `main` 的提交，不使用当前
  工作分支的 `HEAD`，也不包含工作区未提交的改动。
- release tag 的格式为 `x.y.z`，三段均为非负整数；排除 `v1.2.3`、
  预发布和带构建元数据的 tag。
- 仅收集信息和生成文案；不修改版本号或文件，不提交、推送、创建 tag
  或发布 release。用户另外明确要求保存文案时，才写入指定文件。

## 工作流程

### 1. 确定最新 main

1. 检查 `git status --short --branch` 和 `git remote -v`。
2. 通常使用 `origin`；没有 `origin` 且仅有一个远端时使用该远端。
   多个远端且无法确定主仓库时先询问，不猜测。
3. 获取主仓库的最新 main 和 tags，不切换分支、不更新本地 main，
   不强制覆盖 tag：

   ```bash
   git fetch <remote> --tags refs/heads/main:refs/remotes/<remote>/main
   git rev-parse --verify 'refs/remotes/<remote>/main^{commit}'
   ```

4. 将解析到的完整提交哈希记为 `MAIN_SHA`。之后的筛选、日志和 diff
   都使用该哈希，避免执行期间引用移动导致范围不一致。
5. fetch 失败或没有远端时，不声称数据是最新的。询问是否允许使用本地
   引用；获准后明确说明限制。没有可用 main 时停止，不退回当前 HEAD。

### 2. 输出发布文案

先给出单独一行范围信息（不混入正文）：

`比较范围：<BASE_TAG> → main@<短哈希>`

然后在 Markdown 代码块中给出可直接复制的正文，按需要使用以下分组，
删除没有内容的标题：

```markdown
## 新增功能
- ……

## 优化改进
- ……

## 问题修复
- ……

## 升级注意事项
- ……
```

- 将用户价值最高的改动排在前面，每条简洁说明“变化是什么、对用户
  有什么影响”。纯 CI、测试、开发工具及无用户影响的重构通常省略；
  用户要求完整工程变更时才增加“内部维护”分组。
- 不推测下一个版本号；用户已提供时才使用该版本号作为正文标题。
- 存在破坏性变更、配置迁移、依赖或系统要求变化时，必须在“升级注意
  事项”说明实际影响和有依据的操作步骤；不要凭空生成迁移指南。
- 范围为空时明确说明“自上次 release 以来 main 无新增提交”，不生成
  虚构条目；只有内部维护时如实说明，不包装为用户功能。
- 生成前核对每条都能追溯到该范围内的实际改动；有未能核实的事项，
  在正文外单独说明，不将猜测写入可发布文案。
