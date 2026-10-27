# GUI TAB 插件

控制面板和主程序可以加载同一个 `<plugin_dir>/<name>/<name>.so`，但只使用各自的入口：

- `DeskWaifuPluginEntry`：主程序的 `IProcessorPlugin` 工厂。
- `DeskWaifuGuiPluginEntry`：控制面板的 `IGuiPlugin` 工厂。

两种接口均继承公共生命周期接口 `IPlugin`。开发阶段宿主和插件同步重编译，
不维护历史插件二进制或接口布局兼容。

两个入口均为可选；缺少本宿主入口表示该模块不适用于本进程，不是加载错误。
入口只返回静态描述符。宿主先校验结构大小、接口版本、Qt 版本和构建 ABI，
再调用工厂。插件必须与宿主使用匹配的 Qt 和工具链构建，这不是稳定的跨编译器 C ABI。

## 生命周期与页面所有权

GUI 插件在 `Initialize` 中创建页面并注册稳定的局部 TAB ID、显示标题与顺序。
宿主组合出 `<pluginName>/<tabId>` 全局 ID，同序号以全局 ID 排序。
注册成功后，页面所有权转移给宿主；注册失败时页面仍由插件负责释放。
插件初始化时不得发送 IPC 请求；`Start` 成功后才显示页面并开始工作。

关闭或回滚时，宿主先失效上下文，再调用 `Stop`，然后销毁页面、插件实例和上下文。
`Stop` 必须支持部分初始化并停止定时器、结束工作线程，不得抛出异常。
所有 GUI 生命周期操作均在 QApplication 主线程执行。库代码保持映射至进程退出，
不提供热卸载，也不提供插件崩溃隔离。
服务信号的回调接收者必须是上下文、已注册页面或页面的子对象；宿主会在
`Stop` 前清除这些对象上已排队的调用。不带接收者的 lambda 连接，以及生命周期
不受上下文管理的外部接收者，不属于 SDK 的安全拆卸契约。
失效后禁止发送新命令，但已经激活过的上下文仍允许 `cancelQueuedCommand` 清理
尚未发送的命令；取消不能撤销已经发送的修改。插件的 `Stop` 还必须停止自己的
定时器等本地工作，不能只依赖服务失效或稍后的页面析构。

## 业务边界

TAB 是导航分组，不是插件边界；SDK 允许一个插件注册多个 TAB。目前的对应关系是：

| TAB | 所属模块 | 角色 | 功能 |
| --- | --- | --- | --- |
| 系统 | `system/system.so` | 仅 GUI | 状态总览、启停/重启、下次启动的 GPU 选择 |
| 交互 | `interaction/interaction.so` | 仅 GUI | 快捷键、鼠标/手动注视、悬停/平时透明度 |
| 模型 | `model/model.so` | 仅 GUI | 模型、皮肤、表情/动作、外部口型、显示器与变换编辑 |
| 语音 | `speech/speech.so` | 后端 + GUI | 语音提供者与 MiniMax 凭据设置 |
| 日志 | `log/log.so` | 仅 GUI | 被动显示宿主和领域插件发布的安全日志 |
| 无 TAB | `reading_clip/reading_clip.so` | 仅后端 | 剪贴板图像识别与播报编排 |
| 无 TAB | `desktop/desktop.so` | 仅后端 | 模型、渲染、窗口、输入和交互 |
| 无 TAB | `perception/perception.so` | 仅后端 | 图像读取、OCR 和截图 |
| 无 TAB | `ipc/ipc.so` | 后端 + GUI | Unix socket、命令网关和 GUI 传输客户端 |

源码与构建归属以模块为准：`src/plugins/<name>/gui/` 保存所属页面及专用辅助类，
`src/plugins/<name>/CMakeLists.txt` 组装各模块的后端/GUI 角色，
`src/plugins/CMakeLists.txt` 保留公共构建规则与子目录选择。`platform/control/` 只组装控制端宿主，
不再反向给语音模块追加 GUI 文件。进程启动/重启辅助类归 `system/gui`，
模型快捷键控件归 `interaction/gui`。模型、窗口、输入等后台能力由 `desktop`
插件拥有，不由同名 GUI 模块或平台组装。

此布局经过能力所有权讨论后保留现有卡片分布，而非强制“一 TAB 一插件”：

- GPU 是进程级、下次启动生效的配置，保留与重启按钮同页，避免拆开操作流程。
- 交互是输入响应配置，模型页是模型操作和显示编辑，两者不必因为操作同一个桌宠而合并。
- 位置、缩放、裁剪及应用/取消共用完整变换事务，不能拆给不同插件。
- 外部口型幅度并不生成语音，因此仍归模型，不迁到 `speech`。
- 当前没有朗读控件可迁移；不给 `reading_clip` 凭空增加页面，也不让它接管提供者的 API Key。

系统、交互、模型、日志页面由 GUI 插件拥有自己的控件、UI 文件与业务状态。
主窗口只负责 TAB 容器、加载诊断、公共连接状态和通用反馈，不访问插件控件。
语音设置由语音模块的 GUI 入口提供，不再由主窗口实现。

GUI SDK 是基础 SDK 的可选组件；纯后端插件不必链接 Widgets。
但如果同一个模块同时链接了 GUI 和后端依赖，两个进程加载它时都需要满足这些动态库依赖。
因此禁止在模块的静态初始化中创建 QWidget 或启动领域工作。

同一个模块在两个进程中拥有独立实例，不能依靠静态变量共享状态。
GUI 通过 SDK 的通用服务接口调用后端；宿主只代理请求，实际 IPC 传输由 `ipc`
插件注册的 `IGuiTransport` 提供。页面不依赖 IPC 实现，不直接读写后端私有配置或持有后端对象。
日志由页面显式发布，不自动记录所有请求或响应，避免语音密钥进入日志。
通用协议错误、修改结果未知及发送前取消的提示由宿主生成，即使 `log.so` 缺失仍可反馈；
日志插件只订阅 `logMessage`，不承担基础错误处理。

`--smoke-test` 在应用入口独立发送现有 `get_status` 并验证成功响应，不依赖系统页
发送 `backendReady`。仅连接 socket 不算通过；该探针验证命令往返，不声称验证所有页面。
该探针需要 IPC 传输插件，但不需要系统页。readiness 通知仍是公开状态信号，
不作为此 smoke 的成功条件。

## 构建与运行

应用启用 `DESKWAIFU_BUILD_CONTROL` 时默认构建 GUI SDK，独立构建 SDK 时可用
`-DDESKWAIFU_BUILD_GUI_PLUGIN_SDK=ON` 启用它；不启用时仍可只构建 Core SDK。
GUI 插件链接 `deskwaifu_gui_plugin_sdk`，安装后的 CMake 消费者使用 SDK 的
`Gui` 组件和 `DeskWaifu::gui_plugin_sdk` 目标。

系统、交互、模型和日志模块分别安装到 `system/system.so`、
`interaction/interaction.so`、`model/model.so` 和 `log/log.so`。
构建控制面板和语音插件时，`speech/speech.so` 同时导出两种入口。

控制面板从标准配置目录下的 `deskwaifu/config.json` 读取 `plugin_dir`，
缺省值为安装前缀的 `lib/deskwaifu/plugins`（具体目录遵循 GNUInstallDirs）。
`DESKWAIFU_CONTROL_PLUGIN_DIR` 可显式覆盖它，空值禁用发现。
开发时设置该变量为构建目录的 `plugins`，无需复制模块到系统目录。
只加载自己信任的模块；环境变量或配置项不是安全沙箱。

加载器将全局 TAB ID 放在页面的 `deskwaifuTabId` 属性中，不覆盖页面自身的
`objectName`，以保留 UI 查找、自动连接和主题选择器的语义。
