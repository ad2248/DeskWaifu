# Arch 二进制发布

`.gitea/workflows/release.yml` 在 `deskwaifu-arch` 私有 runner 上执行：推送任意
tag（包括包含 `/` 的标签），或手动运行并显式填写已有 tag。不限制标签命名或
版本格式；仍只检出确切的 `refs/tags/<原始标签>`，不接受用分支代替标签或空输入。
checkout action 先获取完整历史/tag，发布工具再 detached checkout 精确 tag；
推送事件还校验事件提交身份。不要求把 checkout 凭据留在仓库中再次 fetch。
实际输入是该提交的 `git archive`，不是触发手动运行时所选的分支。
原始标签直接作为 Arch `pkgver`、包文件名中的版本及 GitHub Release 的标签和标题。
不添加或移除 `v`，不替换字符、不编码、不使用提交号兜底，也不根据标签推断预发行版。
生成 PKGBUILD 时只做 shell 引用以保留原始字符串，绝不把标签当作命令执行。
如果标签不符合 Arch 的 `pkgver` 规则（例如包含 `/`、`-` 或非 ASCII 字符），
由 makepkg 直接报错，任务失败并停止上传；不会为了让打包通过而改写标签。

重跑历史任务仍使用历史工作流与脚本，不会加载后来合并的修复。发布旧源码标签时，
应从包含修复的 main **新建手动运行**，tag 输入仍填写原始标签（例如 `0.1.0`）；
不要重跑旧任务，也不要为此强制移动标签。

检出目标标签前，选择脚本把当前工作流的发布脚本、安装模板和发布测试保存到
checkout 外的独立目录，并通过 `RELEASE_TOOLS_DIR` 传给后续步骤。测试、打包和
上传始终使用该目录中的工具，避免检出旧源码时把修复后的工具换回旧版。
应用源码仍严格来自目标标签的 `git archive`，不会混入 main 的应用代码。

## 产物

- `deskwaifu-bin`：应用、控制面板、命令行工具、全部默认插件；不依赖 OCR 包。
- `deskwaifu-ocr-bin`：原生辅助程序、ONNX Runtime CPU 运行库和许可证；严格依赖
  `deskwaifu-bin=<同一 pkgver>-1`，系统依赖包括 Qt6、OpenCV、yaml-cpp。
  **不包含或下载任何 OCR 模型、字典、Paddle/Python 环境。** 模型须由用户另行配置；
  官方 small recognition 模型目录的 `inference.yml` 内含字典，同样不会进入发布包。
- `SHA256SUMS`、`release-metadata.json`：包含源 tag、完整提交 SHA 和包版本。

主构建显式启用所有默认插件，并使用 `DESKWAIFU_BUILD_OCR=OFF`；原生组件独立
构建，其安装规则不包含模型。安装只调用 `DeskWaifu`、`DeskWaifuOCR`
组件，不发布 SDK 开发文件、私有 SDK 源码归档或模型。

## 依赖与权限

当前固定 CI 镜像尚缺 OpenCV/yaml-cpp。获取依赖步骤使用
`pacman -Syu --needed` 安装完整构建依赖。发布任务必须由 root 编排；非 root
runner 在获取依赖前直接失败，不支持同账号 makepkg 或 sudo 回退。这里不会重建、
推送镜像或变更 runner 固定镜像引用。

`scripts/find-deps-dir.sh` 查找 `/opt/*-deps` 中已有 Cubism SDK/GLEW。
`release-build.sh` 的 acquisition 阶段复制这些依赖、生成 tag 的源码归档，并下载
ONNX Runtime 1.31.0 SDK，验证固定 SHA-256：
`cc5c72baf5ae5c8238a6841f0897227be2d02826b9cf98eaf02fdefaeeb03a57`。
其后 makepkg 的 source 全部为本地归档，均有 SHA-256，不执行下载或安装依赖。
构建需要发行方自行确认 Live2D/Cubism 二进制分发许可义务。

makepkg 永不以 root 运行。root runner 为单次构建创建无 sudo 的临时系统用户，
只把新建暂存目录交给该用户，不对 checkout/workspace 做广泛 chmod/chown。
构建前检查该用户不能写入 checkout、保留工具目录及可信发布脚本，也不能通过
可写的非 sticky 上级目录替换这些目录；若可写则失败，
不修改原目录权限。makepkg 使用清空的环境和独立 HOME，退出时删除该用户及
本次暂存目录，避免构建代码篡改后续会接触上传凭据的脚本。

## GitHub 凭据和重试

Release 任务统一设置 `http_proxy`、`https_proxy`、`ALL_PROXY` 为
`http://100.64.0.11:7912`，`no_proxy=127.0.0.1,localhost`，覆盖 GitHub SDK
下载及 Python GitHub API 通信；runner 必须能访问该代理。

Gitea 仓库 secret **`RELEASE_UPLOAD`** 应为 GitHub fine-grained personal access
token：仅选择 `ad2248/DeskWaifu`，仓库权限 **Contents: Read and write**
（Metadata 的只读权限默认提供）。无需 Actions、Packages 或其他仓库权限。
secret 仅在最后上传步骤注入；checkout、测试、获取依赖、CMake 和 makepkg 都拿不到它。

上传固定使用 GitHub API 的 `ad2248/DeskWaifu`。GitHub 必须已存在完全相同的源提交
（例如由镜像同步提供）；若 GitHub tag 缺失，可在该提交创建 tag，绝不退回 main。
已有轻量/注解 tag 必须解析到同一提交，且不会强制移动 tag。

缺失 release 时先建 draft，全部资产上传并复核 tag 后才发布。重跑跳过字节相同的
资产：优先比较 GitHub SHA-256 digest，无 digest 时下载资产比较。若同名资产大小、
摘要或状态不同，则报错，**不自动删除/覆盖**。构建环境/编译器/归档差异可能导致
重建包不同；这种情况应人工核查，而不是静默替换已发布包。网络/API 错误不当成
“release 不存在”；失败的 draft 可重试。已有公开 release 补传缺失资产不是原子操作。

只运行本地检查：

```bash
python3 -B -m unittest discover -s tests -p test_release_packaging.py -v
bash -n scripts/release-build.sh
```

测试使用离线 CMake 安装替身执行真实 split makepkg 并检查 `.PKGINFO` 与内容；
不等于真实应用/原生 OCR 编译或干净 Arch 安装验证。HTTP 测试完全模拟，不访问发布 API。
此工作流的添加本身不会创建 tag、提交、推送、发布或触发远端流水线。
