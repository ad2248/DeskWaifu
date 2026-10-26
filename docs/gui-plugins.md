# GUI TAB 插件

控制面板和主程序可以加载同一个 `<plugin_dir>/<name>/<name>.so`，但只使用各自的入口：

- `DeskWaifuPluginEntry`：主程序后端接口，保留 ABI v1。
- `DeskWaifuGuiPluginEntry`：控制面板 GUI 接口，独立的 ABI v1。

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

## 业务边界

系统、交互、模型、日志页面由 GUI 插件拥有自己的控件、UI 文件与业务状态。
主窗口只负责 TAB 容器、加载诊断、公共连接状态和通用反馈，不访问插件控件。
语音设置由语音模块的 GUI 入口提供，不再由主窗口实现。

GUI SDK 是基础 SDK 的可选组件；纯后端插件不必链接 Widgets。
但如果同一个模块同时链接了 GUI 和后端依赖，两个进程加载它时都需要满足这些动态库依赖。
因此禁止在模块的静态初始化中创建 QWidget 或启动领域工作。

同一个模块在两个进程中拥有独立实例，不能依靠静态变量共享状态。
GUI 通过宿主提供的 IPC 服务调用后端，不直接读写后端的私有配置或持有后端对象。
日志由页面显式发布，不自动记录所有请求或响应，避免语音密钥进入日志。

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
