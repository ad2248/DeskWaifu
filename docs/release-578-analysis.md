# Release job 578 本机验证

## 原失败与第二个可复现问题

目标任务为 [run 558 / job 578](https://git.kals.top/kals/DeskWaifu/actions/runs/558/jobs/578)。
它是历史任务的第三次尝试，使用提交
`0aee2f5686a4aa947581fc1bc7bd747cdc6cdc7b` 的工作流，在选择标签阶段因
`Expected vMAJOR.MINOR.PATCH` 退出。没有执行依赖安装、真实打包或上传。
后来合并的标签修复不会追溯改变旧任务中的代码。

本机还复现了第二个问题：即便从已修复的 main 运行选择脚本，随后检出历史
`0.1.0` 也会把打包工具换回历史版本，下一步再次拒绝这个标签。
证据保存在 `/tmp/opencode/deskwaifu-release-e2e-npatoiix/` 的
`select.log` 和 `historical-tool-overwrite.log`。

修复是在检出源码前保留当前可信发布工具、模板和测试，后续步骤通过
`RELEASE_TOOLS_DIR` 执行它们。构建账户不能写入或通过可写上级目录替换这些工具。
应用源码仍完全来自原标签；不移动标签、不改变 `pkgver`、文件名或上传标签。
新回归测试先复现缺少保留工具的失败，再验证旧源码与新工具可以独立使用。

## 验证环境和边界

- 基线为已合并 main `ef7b1b3`，加本次工具隔离修复。
- 固定 CI 镜像 ID：
  `sha256:834f983b937f388987d2d17b6b729586a83c82e740f607dc1013bd2b4372e9e4`。
- 容器名：`deskwaifu-release-local-578`；源码使用独立 clone，当前工具只读挂载。
  全量系统升级和包安装仅在一次性容器内执行，不安装到宿主。
- 原始标签：`0.1.0`；原始完整提交：
  `0aee2f5686a4aa947581fc1bc7bd747cdc6cdc7b`。
- 使用工作流指定的代理 `http://100.64.0.11:7912`，
  `no_proxy=127.0.0.1,localhost`。未增加 tmpfs 挂载或放宽超时。
- 编排入口：`/tmp/opencode/verify-release-local.py`；证据目录：
  `/tmp/opencode/deskwaifu-release-e2e-npatoiix/evidence/`。
- 不注入 `RELEASE_UPLOAD`，不提取已有 secret，不执行真实上传。

## 已完成的检查

- 宿主非 root 执行 29 项发布测试全部通过，包括真实 makepkg 夹具、Arch 不兼容
  标签失败、历史源码/保留工具和可写上级目录防替换回归。
- 容器 root 执行保留工具的同一套测试：27 项通过、2 项要求非 root makepkg 的
  夹具跳过；不能把它称为 29 项全部执行。实际 root 编排/非 root makepkg 构建
  由后面的完整发布脚本另行验证。
- `git diff --check` 与 `bash -n scripts/release-build.sh` 通过。
- 通过指定代理，只读查询 GitHub API：目标仓库 `ad2248/DeskWaifu` 已有同一源提交，
  `0.1.0` 指向同一 SHA；公开 Release 查询返回 404。
  这不能证明密钥权限，也不能排除仅认证后可见的 draft。
  证据为上级目录的 `github-readonly-preconditions.json`。

## 真实发布构建结果

从固定镜像执行工作流的完整 `pacman -Syu --needed --noconfirm`，成功安装 OpenCV
5.0.0-13、yaml-cpp 0.9.0-1，并升级 CMake 到 4.4.5-1 等；获取依赖耗时 53.648 秒。
完整版本清单为 `evidence/installed-build-versions.log`。

直接执行本次修复后的 `release-build.sh`，未替换其构建步骤：由容器 root 编排，创建临时
非 root 账户运行真实 makepkg：源码归档、私有依赖、ONNX Runtime 下载和固定摘要
检查、两次真实 CMake 编译、两次组件安装、split package 和产物校验全部成功。
该阶段退出码 0，耗时 74.227 秒，日志为 `evidence/real-root-release-build.log`。

生成的实际产物：

- `deskwaifu-bin-0.1.0-1-x86_64.pkg.tar.zst`，1,375,653 字节。
- `deskwaifu-ocr-bin-0.1.0-1-x86_64.pkg.tar.zst`，10,950,064 字节。
- `release-metadata.json` 的 tag/pkgver 均为 `0.1.0`，commit 为原始完整 SHA。
- `.PKGINFO` 证明 OCR 包精确依赖 `deskwaifu-bin=0.1.0-1`。
- `sha256sum -c SHA256SUMS` 全部通过，真实 uploader 的 `load_assets()` 也已验证
  这批真实产物；没有调用其发布函数。
- 已逐项查看完整包清单：主包含应用、CLI、控制面板及默认插件；OCR 包含 helper、
  ONNX Runtime 和文档/许可证，无 OCR 模型、字典、私有 SDK 源码或头文件。
  `licenses/.../models/` 中只有许可证和 NOTICE，不是模型数据。两个包文件不重叠。
- 两个归档内共 17 个常规 ELF 文件均为 ELF64/x86_64，符号链接目标存在；动态节
  不含临时构建路径。检查结果保存在 `evidence/package-elf-static.json`。
- 构建后的独立源码 clone 的 tracked 文件仍干净，HEAD 与原标签 SHA 均未改变。

## 安装与运行检查的失败记录

两个实际包都已通过容器内 `pacman -U` 安装，不是仅解压后运行。主包未安装 OCR
时实际应用/CLI/控制面板 smoke 通过，OCR 状态为 `helper_missing`；安装 OCR 后
直接运行 helper 返回协议错误 `model_missing`，与无模型交付要求一致。

首次额外运行检查未能启动 KWin：完整升级将 `/usr/bin/kwin_wayland` 的
`cap_sys_nice=ep` 恢复，而 rootless 容器不能执行带该 capability 的二进制，
`dbus-run-session` 报 `Operation not permitted`。原镜像的 Containerfile 已显式
移除该 capability。只在一次性验证容器内恢复这个镜像设置后继续运行；没有改
发布脚本或增加容器权限。首败日志 `installed-compositor-first-failure.log`、
`upgraded-compositor-capabilities.log` 保留。

后续 KWin 检查中还发生连接重置、第二次启动等待超时和退出 255。打开
`QT_FORCE_STDERR_LOGGING=1` 后，日志显示升级后的 KWin 在没有 `/dev/dri` 渲染节点
的容器中报告 `Found no render device`、`We are going to quit KWin now as it is broken`，
KWin 包装进程退出 139，应用因 Wayland 连接断开退出 255。默认 Qt 日志可能被发送
到容器内不可用的 journald，所以此前 compositor 日志为空，不能据此认定没有错误。
这些失败保存在 `runtime-{final,debug,checked,wayland-diagnostic,native-kwin,painter-kwin}/`，
没有放宽应用退出码断言或将这些尝试计作通过。

## 完成的安装运行验证

同一容器、同一批实际包最终改用已有的 Sway headless/pixman 合成器。它以独立
非 root 账户运行在新建私有 runtime 目录，不连接用户桌面，也不需要暴露宿主 GPU。
应用仍使用 Mesa llvmpipe 执行真实 OpenGL 渲染；模型来自容器中的私有 SDK 测试资源，
不在发布包内。只更换额外运行检查的 compositor，未修改发布构建步骤或应用源码。

两种安装状态均已通过：

1. 只安装主包：实际应用启动、`get_status`、CLI、控制面板 smoke 均成功；
   OCR 为 `helper_missing`。
2. 安装 OCR 包且不部署模型：上述检查全部成功；应用通过真实 helper 返回
   `model_missing`，进程保持可响应。
3. 每次启动前执行真实 `wayland-info` roundtrip；两次 `quit` 都回复 ok，应用退出
   码均为 0，日志都包含 GPU 在窗口销毁前释放的标记。
4. 已安装包的 ELF64/x86_64 检查与 `ldd` 全部通过，无 `not found`；源码 clone 的
   tracked 文件仍干净，HEAD/tag 不变。

最终结果为 `evidence/runtime-sway/installed-runtime.json`、`elf-linkage.json` 和
相应应用/合成器日志。本地完整编排最终退出 0；原 KWin 失败记录仍保留。这证明
真实包的安装、渲染与主/OCR 分离行为，不证明无 GPU 容器中的新版 KWin 可用。

本机这部分验证不包含 Gitea 调度服务、checkout action 或认证后的真实 GitHub 上传。
远端 PR 检查、合并、正式 Release 流水线和从 GitHub 下载后的宿主安装应另行核验，
不能用本地成功替代其实际结果。
