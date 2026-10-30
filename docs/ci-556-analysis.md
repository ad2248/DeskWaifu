# CI run 556 失败分析与验证报告

## 1. 结论与责任

本次失败不是主程序编译失败，也不是 CTest 中已有业务断言失败。
直接故障发生在 **故障植入测试为源码副本重新配置 CMake** 时：普通 CI 主构建
显式设置 `DESKWAIFU_BUILD_OCR=OFF`，但 `scripts/check-test-mutations.py`
创建独立构建目录时漏掉这个设置。根工程的新默认值为 ON，意外启用了该测试
并不需要的原生 OCR helper；固定 CI 镜像没有 OpenCV，配置因而失败。

之前的本地构建、CTest、真实 OCR 和打包验证确实执行并通过，但它们**不是完整
CI 的等价执行**。我没有运行故障植入脚本，也没有用固定 CI 镜像复现这条新建
工程的配置路径，因而漏掉了这个问题。责任在改动与验证范围选择，不在用户标签、
上传密钥或 CI 随机不稳定；不能用“本地正常”解释为 CI 出了偶发问题。

## 2. 分析对象与证据

- 原失败任务：[run 556 / job 570](https://git.kals.top/kals/DeskWaifu/actions/runs/556/jobs/570)。
- 对应 PR：[#250](https://git.kals.top/kals/DeskWaifu/pulls/250)，测试提交
  `0c6559d3d41ad721268cedf8cfad6b61f90c1980`。
- 当前 PR：[#251](https://git.kals.top/kals/DeskWaifu/pulls/251)，诊断开始时 head 为
  `3843de4e0d122ef46296485dc5594d532fdd9b2e`。
- 后续核验的 [run 559 / job 574](https://git.kals.top/kals/DeskWaifu/actions/runs/559/jobs/574)
  已确认同样在故障植入配置阶段失败，而普通严格测试门禁通过；原始日志保存于
  `/tmp/opencode/deskwaifu-job-574.log`。因此“这次也会失败”已经由实际结果确认，
  不再只是根据代码推测。
- 上述两个提交之间，故障植入脚本、普通 CI 配置、根 OCR 开关及原生 helper
  的 CMake 依赖声明没有变化；标签修复本身不会消除这次 CI 故障。
- 原始任务日志保存于 `/tmp/opencode/deskwaifu-job-570.log`。
- 复现使用的固定镜像：
  `sha256:834f983b937f388987d2d17b6b729586a83c82e740f607dc1013bd2b4372e9e4`。
- 同镜像复现及修复后验证证据目录：`/tmp/opencode/deskwaifu-ci-556/`。

与 Release run 558 的标签格式失败是两个独立问题。run 556 是 `ci.yml` 的 PR
回归任务，未使用 `RELEASE_UPLOAD`，不存在 GitHub 上传阶段。

## 3. 原任务执行到哪里

以下时间均为原日志的 UTC，状态来自 Gitea job API，而非按本地结果推测：

| 阶段 | 时间范围 | 原任务结果 |
| --- | --- | --- |
| Configure | 15:56:32–15:56:39 | 通过，OCR OFF、普通严格门禁 ON |
| Build | 15:56:42–16:01:02 | 通过 |
| 两种 render-target 分支编译 | 16:01:05–16:03:31 | 通过 |
| Qt/渲染/IPC 普通严格测试门禁 | 16:03:34–16:09:18 | 通过，144 个 CTest 入口，约 341.77 秒 |
| 故障植入门禁 | 16:09:21–16:09:27 | 配置阶段失败，尚未执行六种植入 |
| Sway/headless smoke | — | 被跳过，不能算通过或另一个失败 |
| KWin 实际输入/输出/安装 smoke | — | 被跳过，不能算通过 |
| Debug ASan+UBSan 子集 | — | 被跳过，不能算通过 |

原日志虽显示 `100% tests passed out of 144`，仍单独列出了允许未验证的指定 PCI
offscreen 场景、真实云/音频/私有模型子用例。这个百分比不是全部能力覆盖率。

原在线日志只显示：

```text
mutation configuration failed; see .../build-ci/test-logs/mutations/run-omcykr34
```

它没有打印子配置的具体 CMake 错误。诊断时 run artifacts API 返回空列表，不能
声称已从该 API 下载到原始 `configure.log`；下面的具体错误来自同镜像重放。

## 4. 复现与因果链

在本机已有的**同一镜像 ID**中，以只读源码挂载、独立证据目录及禁用外网的
容器重放：

```bash
cmake -S . -B /evidence/parent -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DDESKWAIFU_BUILD_OCR=OFF -DDESKWAIFU_BUILD_CONTROL=ON \
  -DDESKWAIFU_TEST_GATE=ON \
  -DSDK_ROOT_PATH=/opt/deskwaifu-deps/CubismSdkForNative-5-r.5 \
  -DGLEW_PATH=/opt/deskwaifu-deps/glew
python3 scripts/check-test-mutations.py /evidence/parent
```

父工程配置成功；未修复的故障植入脚本退出 1。证据文件：

```text
/tmp/opencode/deskwaifu-ci-556/parent/test-logs/mutations/run-l16kdqcc/configure.log
```

其中首个致命错误为：

```text
CMake Error at src/plugins/perception/native/CMakeLists.txt:8 (find_package):
  ... CMake did not find ... OpenCV ...
```

原因逐级为：

1. 根 `CMakeLists.txt` 将 `DESKWAIFU_BUILD_OCR` 默认设为 ON，符合全量构建需求。
2. `.gitea/workflows/ci.yml` 的普通回归主构建、渲染变体和 sanitizer 均显式设置
   OCR OFF，因此它们不需要原生推理依赖。
3. `check-test-mutations.py` 读取父 `CMakeCache.txt`，但原本只转交 SDK/GLEW 路径。
4. 子工程使用新的临时构建目录，不继承父缓存，OCR 重新取默认值 ON。
5. 意外进入 native CMake 分支，先要求 OpenCV；镜像没有此包，立即失败。
6. 即使只补 OpenCV，后面仍有 yaml-cpp 和 ONNX Runtime SDK 要求；单补缺包不是
   修复配置意图丢失的正确办法。六种植入均未得到行为拦截或恢复证据。

即使后续 `cmake --build --target` 只选择语音、交互或 GPU 测试目标，CMake 的
配置阶段仍会先处理已开启的 native 子目录；“目标没有链接 OCR”不会自动跳过
该子目录中的 `find_package`，因此必须在独立配置阶段明确限定范围。

引入默认全量构建时，我同步了 `ci.yml`、旧 AUR 主包以及禁用语音的独立配置测试，
却漏掉了这个**由 Python 动态组装 CMake 命令**的入口。原有 `test_ocr_build.py`
覆盖了模型无关构建及直接配置，但没有执行故障植入的独立配置路径；因此这些
相关测试通过，也不能证明没有漏掉其它构建入口。此次新增的配置回归正是针对
这个缺口，而不是重复检查 `ci.yml` 里是否出现某个字符串。

同镜像内查询确认 OpenCV、yaml-cpp 未安装。这不是“必需测试依赖漏装”的结论：
这六种植入不测试原生 OCR 推理，不应把它的依赖引入普通 CI。

## 5. 为什么本地通过不能推出 CI 通过

### 5.1 执行的命令集合不同

之前主要执行的是：

```bash
cmake --build /tmp/opencode/deskwaifu-full --parallel 12
python3 scripts/test-environment.py ctest \
  --test-dir /tmp/opencode/deskwaifu-full --output-on-failure --parallel 8
```

这会运行已注册的 CTest，但**不会自动运行 CI 后面的故障植入脚本**。
普通构建通过也不会检查另一个脚本能否从零配置独立源码副本。

此外，`test-environment.py` 负责隔离 HOME、桌面、D-Bus 与凭据环境，不负责把
任意命令变成完整 CI。运行 CTest 内的 `test-gate` 单元测试，只证明门禁评价器
的测试夹具通过，不会让外层普通 CTest 自动执行 `run-test-gate.py` 的严格检查。

### 5.2 缓存、功能开关与依赖环境不同

| 条件 | 之前本地宿主构建 | run 556 普通 CI |
| --- | --- | --- |
| 构建目录 | 已配置的 `/tmp/opencode/deskwaifu-full` | 独立主构建，植入时另建新目录 |
| Build type / generator | Release / Ninja | Release / Ninja |
| OCR | ON | 主构建 OFF，错误的植入子工程意外 ON |
| `DESKWAIFU_TEST_GATE` | OFF | ON |
| ONNX Runtime | 缓存显式指定本机 SDK | 普通 CI 不应需要 |
| OpenCV | CMake 缓存为 `/opt/cuda/lib/cmake/opencv5` | 固定镜像未安装 |
| Qt | 缓存指向 `/opt/cuda/lib/cmake/Qt6` | 镜像的系统 Qt |
| 验证入口 | 环境隔离 + 普通 CTest | 环境隔离 + 严格报告评价 + 独立植入等后续阶段 |

诊断时查询的工具版本：宿主 CMake 4.4.5、镜像 CMake 4.4.4；两边 GCC 都是
16.2.1，QtCore pkg-config 都报告 6.12.0。**没有证据表明这些版本差异导致故障**；
已证实的触发条件是漏传开关后进入了不该进入的依赖分支。

宿主 `pkgconf opencv4` 报告 4.14.0，但实际旧 CMake 缓存选中的是 OpenCV5 路径，
二者不能混为同一项构建证据。完整环境快照见证据目录的
`environment-comparison.json`。

### 5.3 原验证结果应怎样解释

- 原本地 144 项通过、1 项跳过的陈述，在那个构建和命令范围内成立；本地多出的
  原生 OCR 模型测试入口也说明它并非与 OCR OFF 的 CI 完全相同。
- 真实 ONNX 模型识别、隔离 OCR 应用 smoke、真实 Arch 打包各自提供补充证据，
  但都不覆盖 `check-test-mutations.py` 的独立配置路径。
- 原本地没有执行完整 CI 的两种编译变体、六种植入、完整 Sway/KWin 安装 smoke
  和 sanitizer 子集，不能把这些阶段默认为通过。
- 宿主通常为非 root，而固定 CI 容器内部为 root；部分 makepkg 单元夹具明确只在
  非 root 运行。CTest 进程数相近不代表其所有内部子测试的执行范围相同。

## 6. 本次修复和防回归

1. 在故障植入子工程配置中明确加上 `-DDESKWAIFU_BUILD_OCR=OFF`，使它的测试范围
   与普通 CI 的应用/插件行为门禁一致。不修改默认全量构建设置，不给镜像增加
   无关依赖，也不关闭任何一种植入。
2. 子配置失败时把 `configure.log` 的原因输出到任务错误中，仍保留原始日志，
   避免下次只看到一个在容器删除后难以访问的临时路径。
3. 新增 `tests/test_mutation_configuration.py`：调用实际故障植入入口和真实 CMake
   小工程，证明父 OCR OFF 时不会意外开启子 OCR，并证明致命配置原因会被显示。
   修复前配置用例因 `SystemExit` 异常退出、诊断用例断言失败；修复后两项均通过。
4. 将 `mutation-configuration` 注册到根 CTest 并列入严格门禁 required。
   新测试不能靠漏注册或跳过变绿；该小工程测试不冒充六种真实植入的行为证据。
5. 修正 `docs/testing.md` 的普通 CI 等价配置示例，明确 OCR OFF、严格门禁与完整
   CI 阶段的边界。保留已有的 skip、变异拦截和恢复判定，不放宽断言。

## 7. 修复后的验证记录

本地同镜像构建与测试阶段已完成。以下分别记录首次失败、存储诊断和后续成功，
不把它们改写为“一次完整 CI 全绿”。使用固定镜像、独立构建目录、禁用外网；
源码是以 `3843de4` 为基线加本次修复的
工作树，不宣称它就是历史 run 556 的未修改提交。编排器位于
`/tmp/opencode/verify-ci-556.py`，逐阶段命令、退出码与耗时落在证据目录
`verification.json`，每阶段完整输出为对应 `.log`。

第一轮同镜像重放已通过主配置、编译和两种渲染变体；普通严格门禁中
`mutation-configuration` 也已通过。但 `plugin-manager` 在本机容器存储上触发
30 秒超时，其余入口没有失败。这不是原 run 556/559 的失败点：这两次远端的
`plugin-manager` 均通过。首次严格门禁失败保存在
`parent/test-logs/ctest-ho01gy2w/` 与 `strict-gate.log`，不会被诊断重跑覆盖。
单独对比已确认这是本机容器临时存储的影响：同一镜像、同一测试二进制，默认
容器 `/tmp` 再次超时 30.34 秒；仅改为 `--tmpfs /tmp:rw,mode=1777` 后为 0.01 秒。
宿主挂载证据显示默认存储是 ZFS 上的内核 overlay，并非 FUSE。

独立 Qt 4 KiB `QSaveFile` 探针显示：默认存储每次原子提交约 292–367 毫秒，
tmpfs 约 0.007–0.023 毫秒。独立探针的 syscall 跟踪将慢点定位到 `linkat`
（215–367 毫秒）；普通写入、`fsync`、`fdatasync` 并不慢。因此不能未经测量就
把这次超时解释成“fsync 太慢”或随机线程故障。manager 测试含 100 次原子写入，
这个数量级足以解释其 30 秒预算被耗尽。

边界：跟踪的是独立 Qt 探针，不是完整 manager 可执行程序；没有进一步证明
底层内核/ZFS 的具体缺陷，也没有测量远端 runner 的存储类型。tmpfs 是明确改变了
本地临时存储配置，不是“同镜像就完全同环境”的证明。未修改任何业务代码、
测试超时或 skip 规则。证据为 `plugin-manager-default-tmp.log`、
`plugin-manager-tmpfs.log`、`plugin-manager-environment.log` 及
`plugin-manager-{qsave,syscall}-*.log`。

为继续验证原来被配置错误阻断的阶段，后续诊断从故障植入阶段启动，结果独立
保存到 `verification-mutations.json`。分阶段继续不意味着前面的失败被豁免，
首次失败和后续验证结论均予以保留。

在明确记录 `--tmpfs /tmp:rw,mode=1777` 这一差异后，完整普通严格门禁已重新执行，
退出 0：145 个 CTest 入口中 144 个进程通过、1 个指定 PCI offscreen 场景未验证；
允许的真实云、音频和私有模型子用例继续单独列为 UNVERIFIED。门禁报告 errors
为空，耗时 277.562 秒。证据为 `strict-gate-tmpfs.log`、
`verification-strict-tmpfs.json` 以及 `parent/test-logs/ctest-wj1tfxxd/summary.json`。

目前已完成的独立阶段（同一固定镜像，退出码均为 0）：

| 阶段 | 实测耗时 | 验证结论 |
| --- | ---: | --- |
| 主工程编译 | 40.945 秒 | 通过 |
| USE_RENDER_TARGET 编译 | 9.399 秒 | 通过，并检查正式模型源文件带相应编译宏 |
| USE_MODEL_RENDER_TARGET 编译 | 9.722 秒 | 通过，同上 |
| 普通严格门禁（明确使用 tmpfs 临时目录） | 277.562 秒 | 144 个进程通过、1 个允许未验证，errors 为空 |
| 六种故障植入及恢复 | 59.611 秒 | 六种均成功编译、被指定断言拦截、恢复后通过 |
| Sway/headless smoke | 11.175 秒 | 通过 |
| KWin smoke | 14.115 秒 | 通过 |
| KWin 1.5 倍缩放及安装场景 | 11.277 秒 | 通过 |
| sanitizer 子集编译 | 13.633 秒 | 通过 |
| sanitizer 严格门禁 | 2.578 秒 | 17 个测试入口，报告 errors 为空 |

故障植入结构化结果位于
`parent/test-logs/mutations/run-_mk7oebj/summary.json`；六项的 `caught` 与
`restored_passed` 都为 true。sanitizer 报告位于
`sanitize/test-logs/ctest-p9jmdlh5/summary.json`。这些是实际执行记录，不是仅检查
源代码里存在断言或移除失败项后的结果。

这里复跑的是构建和测试步骤，不模拟 Gitea 调度、checkout action、在线 artifact
服务或 GitHub Release。物理指定 PCI、真实云、发声音频等仍不属于普通软件门禁。
远端 CI 必须在推送本次修复后按新 head SHA 核验；该结论与链接记录在 PR #251
的补充说明中，不能用本节的本地结果或旧 run 状态替代。

## 8. 后续交付要求

- 不能仅凭本地普通 CTest 通过就宣称 PR 的 CI 通过；应核验**最终 head SHA**的
  远端检查结论，并把未执行、跳过和失败分开报告。
- 旧 run 不会因为工作树修复自动变绿；远端通过需要把修复交付到 PR 并由新提交
  执行新的 CI。不得用旧提交的检查状态替代新提交结果。
- 本报告不推断谁或什么策略允许了失败 PR 的合并；日志只能证明原 run 失败及
  PR 已合并，不能据此声称仓库配置了或没有配置某种分支保护。
