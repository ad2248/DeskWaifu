# 回归证据与门禁（#242）

测试按产品承诺解释证据，按运行能力执行。不以数量、源码字符串或一次成功代替覆盖结论。
继续使用 CTest、QtTest、Python；没有自建 Qt/Socket/EGL 调度模拟器。

## 本地入口

完整门禁需要 Linux、CMake 3.21+、Qt/LayerShellQt、Mesa llvmpipe、D-Bus、Node、Wayland scanner、Cubism SDK 与 GLEW。
SDK/GLEW 可位于源码外，通过以下参数指定，不必复制到仓库：

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DDESKWAIFU_BUILD_CONTROL=ON -DDESKWAIFU_TEST_GATE=ON \
  -DSDK_ROOT_PATH=/path/to/CubismSdkForNative-5-r.5 -DGLEW_PATH=/path/to/glew
cmake --build build --parallel 8
python3 scripts/test-environment.py python3 scripts/run-test-gate.py build

# 单场景不冒充完整门禁；可直接检查故障的实际输出。
python3 scripts/test-environment.py python3 scripts/run-test-gate.py build --test '^speech-contract-queued-replace$'
python3 scripts/test-environment.py python3 scripts/run-test-gate.py build --test '^interaction-opacity$'
python3 scripts/test-environment.py python3 scripts/run-test-gate.py build --test '^model-render-thread$'

# 平台效果：各自启动私有 compositor，不注入当前桌面。
DESKWAIFU_DEPS_DIR=/path/to/deps bash scripts/headless-smoke-test.sh build
python3 tests/qt_headless_smoke.py build /path/to/CubismSdkForNative-5-r.5
python3 tests/qt_headless_smoke.py build /path/to/CubismSdkForNative-5-r.5 --scale 1.5 --installed

# 受控源代码副本：故障不得进入当前工作树或正式提交。
python3 scripts/test-environment.py python3 scripts/check-test-mutations.py build
```

`DESKWAIFU_TEST_GATE=ON` 拒绝关闭测试/控制面板以及缺失的 Python、D-Bus、Node。
`tests/gate.json` 额外核对关键注册入口。所有实际注册的测试都执行；不在清单里的新测试也不能任意 skip。
纯设备/授权子用例使用精确的类/用例名标注原因，不能将整个 QtTest 进程列为例外。
普通构建可以关闭门禁选项；不能把它的成功当作完整门禁通过。

## 能力矩阵与调度

| 能力/入口 | 执行策略 | 真实依赖 / 替换依赖 | 证据边界 |
| --- | --- | --- | --- |
| 根 CTest，由 `run-test-gate.py` 执行 | 每个 PR 合并必跑，Release | 生产状态机、Qt 循环、线程、Unix socket、本地 WebSocket、私有 D-Bus；外部服务用夹具 | 不证明真实云、真实音频或实际 OCR 模型推理 |
| `speech-contract-*`、`interaction-opacity`、`model-render-thread` | 每个 PR 必跑；故障植入也必跑 | 见三个实验 | 不用测试副本当生产策略证据 |
| `ocr-client` | 并入根 CTest，每个 PR 必跑 | 生产 OcrClient + 真实 native 子进程/协议；替换 OCR 引擎 | 没有下载模型，不验证 OCR 准确率；独立 `features/ocr/tests` 工程仍可使用 |
| `aur-packaging` | 并入根 CTest，每个 PR 必跑 | 包装文件元数据约束 | 不是 makepkg 安装或干净系统依赖验证 |
| Sway smoke | 每个 PR 合并必跑 | 私有 Sway、Wayland、真实应用/控制面板/IPC、多输出 | 无键盘 seat；不拿它代替 KWin 焦点或真实点击穿透 |
| KWin smoke，scale=1 和 1.5 | 每个 PR 合并必跑 | 私有 KWin/D-Bus、真实 layer-shell、窗口像素、焦点、热插拔、帧/IPC、实际点击路由 | 软件驱动/虚拟输出，不证明物理显示器与所有桌面效果 |
| KWin `--installed` | 每个 PR 的 1.5 倍缩放场景必跑 | 私有 prefix 安装组件，使用安装的三条命令；源码外 cwd；运行控制面板 smoke | 复用 CI 镜像系统库，不是全发行版兼容矩阵；OCR helper/模型是独立可选安装，不虚报已安装 |
| `USE_RENDER_TARGET` / `USE_MODEL_RENDER_TARGET` | 每个 PR 两种分支编译必跑 | 正式应用目标 | 编译成功不等于两种路径的像素行为均已执行 |
| ASan+UBSan | 每个 PR 核心契约子集必跑，Debug | 事件、语音/交互契约、ConfigStore、IPC 背压 | SDK 的预编译 Core 未插桩；不宣称 GL/Qt/线程检查全覆盖 |
| `offscreen-renderer-test`、`gpu-transfer-test` 的指定 PCI 场景 | 专用 GPU runner；发布前由维护者显式执行并附报告 | 必须给出具体 PCI 地址、驱动版本和窗口 GPU；跨 GPU 分别记录 | 普通软件门禁中的 auto **不能**充当指定 PCI/跨 GPU 证据；普通报告明确未验证 |
| AudioPlayer 设备场景、真实云 API、私有模型 | 专用/授权验证；未获授权不运行，不阻断普通软件 PR | 明确的设备、key 文件、可发声授权或模型目录 | 不因继承环境变量自动开启；PCM 无设备验证继续由 #230 交付，设备选择由 #233 交付 |

KWin 和 Sway 缺能力要失败，不能返回 77 把必跑平台涂绿。`kwin-cursor-locale` 无法创建私有 locale 时仍保留原来的 77，完整门禁会据此失败。
`offscreen-renderer` 的进程级 77，以及清单内的 QtTest 子用例 skip，单独输出 `UNVERIFIED` 和原因；其他 skip 使门禁失败。
例如启动器并发测试检测到同 UID 已有真实桌宠时，不能悄悄放过：门禁失败，需在独立进程环境重现。
CI 将 CTest、Sway、KWin 顺序执行，避免本次 smoke 自己干扰 `/proc` 进程发现测试；本地也不要并行启动这些完整入口。

## 三个设计实验

### B：取消、替换与迟到结果（先实施）

产品承诺：同一请求按契约终结一次，回复原接收者；取消/替换后旧结果不得开始或污染后续语音，不能只检查最终 Idle。

生产路径为 ReadingClipProcessor → SpeechProcessor → SpeechPlayer → source/audio；图像和 OCR 边界使用可控制的能力夹具。
`speech-contract-reading-complete/reading-cancel` 用真实朗读/语音状态机检查状态序列、图片释放与后续副作用。
`processor-pipeline` 另有真实图像编码/OCR 子进程、OCR 取消与 PNG 生命周期证据，不用内存夹具代替真实子进程语义。

独立场景覆盖：正常 EOF 必须等到 Drained；Preparing/Playing/Draining 取消；无权取消；替换；
旧任务仍活跃时两次连续替换只启动最后一个；队列交错时更新的替换不能被旧请求反向覆盖；
旧结果在新任务后到达；重复终结；语音适配器销毁；请求者销毁；朗读正常/取消。
每次终结断言 sender、receiver、type、次数，source/audio 记录真实调用边界和任务 ID。

时序控制使用已有 source/audio 虚方法与信号，无需新增生产访问器。数据流为：
`Speak → Accepted/Source.Start → 手动完成/取消/替换 → 实际 Finished → 定向终结 → FIFO fence`。
只在完成通知已观察到、或明确投递的一跳事件之后使用 `EventLoopFence.hpp`；它不能等待未来的 worker/socket 事件，也不是“处理 N 次事件”。
语音 source/audio 被替换，不验证云协议兼容或发声；本地 WebSocket 测试另证源协议。

实验发现并修复：Finished 中重新排队旧替换会让它落在更新请求之后，再覆盖新任务。现在在本域接管已接受替换的状态，仍由真实 Qt 状态机异步启动。
故障植入：去除 SpeechPlayer 的旧 ID 过滤 → `speech-contract-replace` → `old results cannot affect the new request`。
#236 的适配层缺口由这些场景补充；不改造它之外的音频缓冲专项。

### A：悬停透明度

产品承诺：生产 `PrepareFrame()` 根据焦点、当前命中快照、开关、独立 idle/hover 值与编辑手柄求透明度，不受旧模型快照影响。
`interaction-opacity` 运行真实 InteractionProcessor、TransformEditor、输入监听和事件循环；窗口/模型命中数据是显式小夹具，不创建 GL。
同时检查全局光标采样只改变视线，不合成拖动、局部指针位置或编辑行为。

| 原辅助断言 | 迁移状态与现有证据 |
| --- | --- |
| 未进入/进入/离开，离开后仅移动不恢复焦点 | 已覆盖：Leave/Enter/Position → PrepareFrame |
| 禁用，hover=0/.01/.5/1，多帧，idle=0/.25/1 | 已覆盖：生产入口参数表；答案是独立常量 |
| 光标静止，模型区域消失/恢复，区域外点 | 已覆盖：可变 CPU 命中边界；模型像素与真实快照生成分别由 model-region-render/model-render-thread 验证 |
| 手柄与模型重叠、非重叠 | 已覆盖：真实裁剪事务生成 handles，角点/内部点分别断言 idle/hover |
| 隐藏、旧模型 token | 已覆盖：服务隐藏和不匹配快照下回到 idle |
| 输入轮廓提取/裁剪/阈值、配置合法性 | 保留原 HoverOpacityTest、ModelInputRegionTest、Settings 测试的生产断言 |
| 渲染后像素与 GL 状态 | 保留 ModelRegionRenderTest；仅去掉重复策略推算，使用明确的数学透明度期望 |

故障植入：把生产 hover 分支强制变成 idle → `interaction-opacity` → `frame.opacity == hovered`。
该实验证明策略，不证明透明像素的鼠标真实穿透；后者由 KWin `click-through-test` 的上下两个真实窗口及 compositor fake-input 路由证明。
点击注入使用实际输出全局原点，不假设 screens()[0] 位于 (0,0)；收到了目标窗口事件才检查另一窗口没有副作用。

### C：GPU 生命周期

产品承诺：实际模型线程中资源操作与释放使用有效所属上下文；停止、部分初始化失败和绑定失败后资源边界正确，能够恢复。
`model-render-thread` 使用真实 ModelProcessor/ModelRuntime、Cubism、软件 EGL、纹理上传和释放；包装 ModelRenderTarget 与 GL 纹理函数只记录，仍调用真实实现。
记录实际创建/删除的纹理 ID、数量、QThread 与 EGLContext；停止后检查无遗留。
覆盖 Prepare 失败、上下文前/后初始化失败、无模型初始化失败、帧绑定失败、恢复及旧 callback 不能复活。

| 历史断言 | 迁移状态与限制 |
| --- | --- |
| 两处可能被 NDEBUG 移除的 current/零驻留检查 | 用真实 GL 创建/删除账本、线程和上下文检查替换，Release 也执行；保留生产 TextureResidency 租约/共享/失败加载断言 |
| 完整六阶段私有析构顺序 | 不再适用：生产组装已改为 processor/runtime 所有权；只保留产品要求的有效上下文与资源释放，不锁死任意全序 |
| 无 GPU 资源时不绑定 | 部分覆盖：Prepare/BeforeContext 失败不进行 GL 纹理操作；没有独立于渲染上下文的“仅启动 Cubism runtime”生产入口 |
| 绑定失败时不错误删除 | 覆盖帧绑定失败后的正式收尾和恢复；不等价于物理 context-loss/驱动复位，后者列入硬件发布证据 |
| 传输先销毁，服务仍可接收断开 | 保留 IpcTransportTest 的真实传输析构断言，直接销毁真实对象；processor-ownership 覆盖晚到领域完成 |

负向隐藏断言等待三个实际产出的透明帧，再检查模型 Update 计数没有变化，而非 sleep 后碰巧状态正确。
GUI 不派发时独立绘制使用不处理 GUI 的有界条件等待。移除普通正确性测试中的帧吞吐上限，不把机器速度当作语义。
检查真实纹理和目标调用，不宣称每个驱动内部对象、所有 shader/FBO 操作都已单独拦截。
故障植入：跳过生产 runtime Shutdown → `model-render-thread` → `calls.created == calls.deleted`。

## 推广到其余风险

- 状态与持久化：ConfigStore 的解析、冲突、权限、符号链接与原子替换；TransformEditor 的非法输入、事务、失败预览/恢复、时钟驱动 debounce。
  新增真实 rename 边界故障，检查失败响应、输出快照/磁盘不变、无临时文件泄漏、恢复后可重试。不将其外推成断电耐久性或对不合作外部写入者的完整 CAS。
- 模型身份：真实线程验证无效切换不改变状态、同模型选择保留 token，KWin/IPC 验证保留控制；实际切换清除旧控制/拒绝旧命令。
  新必跑 KWin 测试暴露同模型选择意外重置 token，修复为不重建同一活动会话。
- IPC：IpcBoundedTransportTest/processor-pipeline/processor-ownership 保留分片、预算、半关闭、慢客户端、其他连接进展、无渲染帧时继续处理、传输销毁后晚完成；KWin 增加实际帧提交不受慢客户端影响。
- 桌面：软件窗口像素、真实 layer-shell 输入区域协议与点击路由各自独立，不混成一个“桌面已覆盖”结论。
- 构建组装：`deskwaifu_application` 是生产与三个集成测试共享的静态目标；窗口/入口单独链接，不再从 executable 的 SOURCES 提取再用文件名排除。必要的私有符号包装保留，并明确观察范围。

## 隔离、失败产物与维护

`test-environment.py` 用环境白名单创建私有 HOME/runtime/config/cache/data/tmp 和 D-Bus，移除继承的桌面套接字、插件覆盖、凭据、发声与付费测试开关。
在 GLVND 系统选择 Mesa 的 vendor JSON，避免仅靠 `LIBGL_ALWAYS_SOFTWARE` 却跑到 NVIDIA。
退出/失败/中断清理自己的进程组和临时根。它是测试环境隔离，不是网络或文件系统安全沙箱；测试仍须只用合成输入和本地边界。
Sway 不再删除传入目录里的 `wayland-*` 或应用 socket。回归用真实 Unix sentinel socket 验证用户目录保持不变。
源码中的合成 key 不是真凭据；常规测试没有真实云目标的有效凭据，也不读取用户配置。授权测试需从明确的专用入口单独调用，不通过普通 launcher 继承开关。

每次 CTest 运行保留独立的 `BUILD/test-logs/ctest-*/{registry.json,ctest.xml,ctest.log,summary.json}`，不覆盖首次失败；包括实际注册命令、首次退出状态、每场景耗时和未验证原因。
KWin 日志位于 `BUILD/qt-smoke-logs/<scale>/run-*/`，包括实际点击、窗口、GPU 传输、模型、安装与控制面板输出；Sway 位于 `test-logs/sway-*/`。
故障植入记录 `test-logs/mutations/run-*/` 的故障、入口、目标断言、编译结果、失败日志及恢复成功日志。
CI 无论成功失败都上传这些文本产物；不上传 SDK、模型、构建二进制、环境全量或真实凭据。新增图像差异/随机测试时应记录合成输入、独立期望和种子，并先检查产物是否含隐私。

增加保护时：先定义可观察承诺和真实/替换边界 → 注册独立场景 → 需要关键入口保护时加入 gate.json.required → 设计一次针对性故障 → 验证指定断言失败且恢复后通过。
新增 skip 必须说明为什么是可选能力，精确限定到用例；不得扩大整个套件的例外来修绿 CI。
共享设施只在重复使用后提取；当前只共享一个语义明确的 FIFO fence，没有万能时钟/事件/窗口框架。
诊断重跑应另存日志，不能把首次失败覆盖成“稳定通过”。

## 实测基线与预算

初始 `f311447`：本机 Release 构建成功；84 个 CTest 入口串行约 246.64 秒，model-pointer-drag 失败、指定设备 offscreen 未验证。
这次基线发现旧夹具阻塞异步输入、未接入的 AUR 元数据不一致、KWin 同模型 token 回归；不把它称为历史稳定性测量。
本次另外实际执行三个行为故障植入，均由预期断言拦截且恢复通过；详见同次交付的报告。
CI 保留原有 30 分钟总时限。单次本机基线不能推出 p95/误报率或生产资源上限；先记录每次 summary 耗时，再由维护者根据 CI 多次运行调整预算，不设虚构的覆盖率/测试数指标。
