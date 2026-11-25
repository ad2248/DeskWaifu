# AUR 打包与维护

AUR 配方在 `packaging/aur` Git 子模块中。GitHub Release 提供 `deskwaifu-bin`、`SHA256SUMS` 和 `release-metadata.json`。发布工作流不需要初始化 AUR 子模块。 [子模块地址](<../../.gitmodules>) · [发布打包](<发布打包.md>)

## 更新

发布资产可公开下载后，初始化子模块并确保检出干净。准备 Python、makepkg 和 bsdtar，以普通用户运行更新器。将示例中的 `0.1.0` 替换为要打包的 tag。

```sh
git submodule update --init packaging/aur
python3 scripts/update-aur.py 0.1.0
```

更新器接收符合 Arch `pkgver` 规则的精确 tag。它核对源提交、清单、校验和、包载荷及依赖，再更新 `PKGBUILD` 和 `.SRCINFO`。配方只提取上游包中的 `usr/` 载荷。 [更新器](<../../scripts/update-aur.py>) · [配方模板](<../../packaging/aur-template/PKGBUILD>)

- 接受单包配方，以及包含 `deskwaifu-ocr-bin` 的 split 配方输入。对 split 配方，必须选择不同于其中 `_tag` 的已发布 tag，更新器使用模板输出单包配方。若模板或现行配方缺少上游运行依赖，更新器会在写入前报错。
- 使用 tag 原文作为 `pkgver`。改变 tag 时 `pkgrel` 重置为 1，同一 tag 更新保留 AUR 的 `pkgrel`。同一 tag 的上游校验和变化会被拒绝。
- 审查生成的差异，在干净 Arch 环境以 `makepkg -s` 构建并核对包和 `namcap` 结果。先提交并推送 AUR 配方，再更新主仓库的子模块指针。维护凭据只用于 AUR 推送。
