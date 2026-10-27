# 测试归属迁移

普通测试的根入口为 `platform/tests/CMakeLists.txt`，脚本门禁仍位于 `tests/`。

- desktop、perception、IPC、reading_clip、speech 的私有单元测试分别位于自己的 `src/plugins/<owner>/tests`。
- `processor-pipeline` 保留为聚合门禁：依次执行 reading、perception、IPC 领域适配器，再动态加载 perception/reading 做完整链路；speech 开启时保留无凭据失败覆盖。
- `processor-ownership` 动态加载 IPC/perception/reading，验证异步结果、半关闭、外部 OCR 接受/查询/取消、重复键拒绝及卸载。原 desktop 窗口/输入/编辑保护/会话释放覆盖在 `desktop-ownership`。
- `gui-tabs` 仅通过 GUI SDK 与动态插件交互；不编译插件 GUI 源码。`control-theme` 通过公开宿主消息填充真实热键面板。
- `MainWindowTransformTest` 不包含 desktop 私有头。其原设置假后端被移入 desktop 自己编译的 `desktop-settings-fixture` 模块，测试仅通过 test-only JSON 协议控制并观察。
- `platform/testing` 只供 platform 集成测试使用；领域单元测试仅依赖 SDK、自己的实现和目录内的小夹具，不为测试放宽构建防火墙。
- speech-reading 与 respawner 的跨插件覆盖位于 platform；speech/system 自编译测试模块通过公开测试协议暴露可控的 source/audio 或重启边界，platform 不包含它们的私有头。
- `build-boundaries` 独立创建无需 Qt/Cubism 的有效 C++ 夹具，验证真实防火墙拒绝跨归属源文件、链接（含 interface/generator expression）和绝对/相对/尖括号 include，并验证合法消费者可构建。
- `delegate-keyboard` 保留 desktop 私有键盘、失败日志、重复按键抑制及模型令牌断言；`delegate-plugin-commands` 位于 platform，动态加载生产 DesktopPlugin 入口（仅窗口换成 EGL 测试边界）、真实 IPC/speech 模块，验证完整命令管线、配置隔离与 quit 最后回复。

## 验证记录

已通过：`python3 tests/test_build_boundaries.py`（5 项），`python3 tests/test_plugin_boundaries.py`（7 项），独立 `src/plugins/perception/tests/ocr-client` 配置、构建与 `ocr-client` CTest。

迁移后的 79 个测试/夹具 C++ 源文件已做独立语法检查（QtTest 的 moc 与 Wayland 夹具头在临时目录生成）；这是语法证据，不替代正式目标链接与运行。另通过 `platform/tooling/BoundaryTest.py`（4 项）和 `tests/test_test_gate.py`（7 项）。

首版提交的全量目标配置、编译及普通门禁已通过：134 个注册测试中 133 项通过，1 项按政策
标记未验证。明确 PCI 设备、私有模型、付费云端和可发声音频的授权测试未执行，不能
据此宣称这些环境已覆盖。报告位于构建目录的 `test-logs/ctest-*/summary.json`。

全部业务模块及 GUI 关闭的纯平台配置完成构建，27/27 项测试通过，不需要 Cubism、
GLEW、LayerShellQt、OCR 或语音专属依赖。SDK/平台宿主独立配置完成构建，9/9 项契约
测试通过，包含安装后消费者和 GUI 传输失效屏障。

禁用全部后端模块的 GUI 配置完成构建，32/32 项测试通过。另在全新目录仅构建
`deskwaifu-control` 目标，确认没有产生任何业务插件 `.so`，空插件 GUI 宿主能启动。

`speech-plugin-load` 已归平台测试。`delegate-keyboard` 不再使用 socket 或可选插件装配；
平台集成使用 `desktop-keyboard-fixture`，编译真实 `DesktopPlugin.cpp` 和正式 desktop
core，只替换窗口边界。两项 Delegate 场景各自连续重复 5 次通过。

独立 KWin 虚拟双输出的 Qt/模型/IPC/退出流程已通过正常缩放，以及私有安装前缀下的
1.5 倍缩放验证。Sway 冒烟因环境未安装 `sway` 未执行，不能报告为通过。测试没有
接入用户真实桌面、真实剪贴板、云端服务或可发声音频。

## 首轮严格审查修正

- 桌面 alpha/命中/缩放策略与帧交换收回 desktop 私有实现，原算法断言随归属迁移。
  新增 `plugin-sdk-image-file` 与安装后仅 QtCore 消费者，验证共享文件寿命不依赖 perception。
- 构建防火墙覆盖 C/C++，在链接前对生成的实际产物清单校验归属；新增自定义输出目录、
  `-Wl`/`-Xlinker`、响应文件、库搜索及对象文件负例，以及合法 launcher 和重命名供应商目录正例。
- `desktop-commands-fairness` 动态加载真实桌面命令桥，仅以本插件夹具控制领域完成；验证
  慢模型期间状态/隐藏/退出继续前进，双向取消占位、后续结果关联及 Stop 屏障。
- `perception-startup` 通过公共加载协议在 perception Start 前请求状态，并在启动后反复验证
  状态、取消、超时及回复配对；早期请求明确失败，不进入相关队列。
- `qt-desktop-window-run` 运行真实 Run/StartFrames，仅替换窗口可用性和 GL 边界；验证外部
  退出嵌套循环后不再调用引用栈局部的定时器回调，并覆盖重复运行、普通失败及不可用窗口。
- `plugin-manager` 覆盖初始化/启动失败与 Stop/析构失败的四种组合；回滚失败出现在启动
  诊断中，并保留到最终 Stop 返回失败，健康插件与后续生命周期不受旧故障状态污染。
- 显式声明 manager 与 OCR 测试的动态模块/子进程夹具依赖，单独构建测试目标也能运行。

上述新增运行时回归已证明在修复前失败、修复后连续 20 次通过；25 项定向 ASan/UBSan
通过，启用了栈返回后使用与泄漏检测。对应报告：
`build/architecture-sanitize/test-logs/ctest-w7ajcrnq/summary.json`。

修正后重新构建全部目标，完整普通门禁 138 项中 137 项通过，1 项仍按明确 PCI 设备授权
政策未验证；没有扩大全局或子用例 skip 例外。完整报告：
`build/architecture/test-logs/ctest-i7r66jkv/summary.json`。
两个渲染目标编译变体以及 KWin 构建目录 scale=1、私有安装目录 scale=1.5 冒烟再次通过。
全新纯平台配置 26/26、无后端 GUI 配置 31/31 通过；计数较首版各减少 1，是两个桌面
算法测试移回 desktop、同时新增一个 SDK 文件寿命测试的净变化，不是删除回归。
六项行为变异再次全部被指定断言拦截，恢复后均通过，报告：
`build/architecture/test-logs/mutations/run-g500j45w/summary.json`。
