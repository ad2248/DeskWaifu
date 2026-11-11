# AUR 打包与维护

> 基线：`0058b7e`。本文区分规范要求、现行设计与证据边界：约束性措辞保留原文要求；现行行为和限制不自动升级为规范；操作指南不代表已执行或验证。以下命令仅供维护者使用，本次迁移不初始化子模块、不构建、不提交、不推送。

## 所有权

- [`packaging/aur/`](../../packaging/aur/)：指向 `ssh://aur@aur.archlinux.org/deskwaifu-bin.git` 的 Git 子模块。其拆分式 `PKGBUILD` 使用 GitHub 二进制产物，输出 `deskwaifu-bin` 和 `deskwaifu-ocr-bin`。
- [`packaging/arch/deskwaifu-git/`](../../packaging/arch/deskwaifu-git/)：跟踪 GitHub `main` 的本地源码构建配方，不是第二个已发布的 AUR 仓库。
- [`packaging/release/PKGBUILD.in`](../../packaging/release/PKGBUILD.in)、`scripts/release-*` 及 [`.gitea/workflows/release.yml`](../../.gitea/workflows/release.yml)：负责二进制产出和 GitHub 上传。它们留在主仓库，不要求 AUR 子模块。
- [`scripts/update-aur.py`](../../scripts/update-aur.py)：验证已发布发行版并编辑 AUR 配方，不构建二进制、不提交、不推送。

普通克隆、源码构建及 CI 不需要递归检出子模块。维护者使用自己的 AUR SSH 密钥初始化子模块：

```bash
git submodule update --init packaging/aur
```

没有 AUR SSH 凭据时，可只读初始化：

```bash
git -c submodule.packaging/aur.url=https://aur.archlinux.org/deskwaifu-bin.git \
  submodule update --init packaging/aur
```

不得将 AUR 私钥暴露给 PR 任务、构建或依赖获取流程。

## 更新发行版

1. 先完成现有 Gitea 发行工作流。两个二进制包、`release-metadata.json` 和 `SHA256SUMS` 都必须可公开下载。
2. 检出最新 AUR 分支，确认子模块干净。安装 Python、makepkg 和 bsdtar 后，以普通用户运行 `python3 scripts/update-aur.py 0.1.0`（替换为精确标签）。
3. 审查差异。更新器核对 GitHub 标签提交与清单、SHA-256、二进制包身份、依赖及载荷契约。同一已存在发行版若校验和不同，会被拒绝。如果发行版要求的依赖未包含在生成的 AUR 元数据中，更新器会在修改任何一个受维护文件之前拒绝更新。此时手动调整配方、重新生成 `.SRCINFO` 并提交这些变化，再重试。
4. 在干净 Arch 环境运行 `makepkg -s`、`namcap PKGBUILD`，并对每个生成包运行 `namcap`。先单独测试主包，再测试 OCR。只需要主拆分包时，不要使用 `makepkg -i`。
5. 先提交并推送 AUR 仓库，再在主仓库提交其新指针。不得发布 AUR 无法提供的父仓库指针。

```bash
git -C packaging/aur add PKGBUILD .SRCINFO
git -C packaging/aur commit -m 'feat(aur): 更新二进制发行版本'
git -C packaging/aur push origin master
git add packaging/aur
git commit -m 'chore(aur): 更新子模块版本'
```

GitHub 标签当前原样映射为 `pkgver`，上游产物使用 `pkgrel=1`。不支持的标签语法会被拒绝，而不是静默规范化。AUR 的 `pkgrel` 独立：仅修复配方时递增，并通过 `makepkg --printsrcinfo > .SRCINFO` 重新生成 `.SRCINFO`。OCR 依赖仅声明主包名 `deskwaifu-bin`。新上游版本将 AUR `pkgrel` 重置为 1；更新同一发行版时保留它。

下载使用 `upstream-` 文件名前缀，避免与 makepkg 输出包冲突。`noextract` 防止两个上游归档互相覆盖；每个包仅提取自己的 `usr/` 载荷。不安装上游包元数据，并禁用符号剥离和调试包拆分。OCR 许可证目录从上游组件名 `deskwaifu-ocr` 重命名为安装包名 `deskwaifu-ocr-bin`。

自 `0.2.1` 起 OCR 使用系统 ONNX Runtime：OCR 包必须声明 `onnxruntime-cpu` 运行依赖，其载荷只含模块与模型许可证，不再内置 `libonnxruntime` 或运行库许可证，因此配方不安装任何 `onnxruntime/` 许可证文件。

已发布载荷继承的已知 namcap 提示：`/usr/libexec/` 是有意使用的，因为应用在该处调用 OCR；在 AUR 配方中移动它会破坏契约。运行时加载的 Qt Wayland 组件、外部辅助程序依赖及 gcc-libs 可能被静态检查报告为不必要，仍有意保留，以符合发行契约。

## 失败与迁移安全

- GitHub 发行版不完整或检查失败时，不得改变 AUR 配方。
- AUR 推送失败可以重试，不需要重新构建或替换 GitHub 产物。
- 保持 GitHub 发行版不可变；需要变更时发布新版本，不替换文件。
- 绝不强制推送已有 AUR 历史。
- 替换现有 `packaging/aur/` 检出之前，检查受跟踪修改及被忽略的构建产物。仅在所有者同意后移动或删除缓存。
- 公开发布之前，验证 Live2D Core/SDK 衍生二进制的再分发权。已有发行产物不是许可授权的证据。

安装或切换包不得删除模型或用户配置，不得启用自启动。源码变体和二进制变体互相冲突，不能覆盖安装。OCR 模型/词典单独配置，绝不包含在这些包内。

## 需求验收与证据边界

- 维护者按指南验证发行完整性、身份/依赖/载荷、干净环境构建与 namcap 结果，保留已说明的载荷契约及提示解释。
- 核对失败不修改配方、AUR 先推送再提交父仓库指针，以及用户数据、自启动和密钥隔离要求。
- 发布前另行确认再分发许可；本文不以既有产物替代授权证据，不宣称已执行任何 AUR 操作。
