# 插件边界与迁移范围

插件按 `<plugin_dir>/<name>/<name>.so` 发现，不读取 manifest，不递归扫描。
`plugin_dir` 是主配置中的宿主字段。配置路径传给插件的是
`<主配置文件所在目录>/plugin/<name>/`；插件自行决定文件名、schema 和读写时机。
配置目录以传入的配置文件路径的父目录为准，不追踪配置文件符号链接到另一处目录。
默认插件目录位于安装前缀的 `lib/deskwaifu/plugins`；也可以配置为其他受信任目录。
空的 `plugin_dir` 禁用插件发现。动态库和目录的符号链接不参与发现。

## 开发阶段与信任模型

- 使用同一发行版本的 C++/Qt SDK 构建；不承诺跨工具链的稳定 C ABI。
- 宿主和插件随源码同步重编译，不维护历史 ABI 兼容层。
- 插件是与宿主具有相同权限的受信任代码，同进程不能隔离崩溃或死锁。
- 启动时发现和初始化，运行期间不热加载、重载或卸载。
- 退出时先停止任务和生产源，再销毁实例。动态库保持映射至进程退出，
  避免 Qt 元类型、回调或共享资源的析构函数指向已卸载代码。
- 目录遍历顺序不能决定能力提供者。没有提供者、提供者不唯一时应明确失败。
- 失败插件只回滚自身，诊断不会阻止宿主和其他健康插件继续启动。

## 分层

宿主和核心 SDK 只知道插件生命周期和事件基类，不解释语音、OCR 等领域数据。
领域事件契约由生产者和消费者共同依赖，不能反向依赖具体实现。
事件通知与请求要区分：通知不因一个订阅者返回已处理而截断；请求需要关联身份、
明确的终态，以及取消和超时语义。高频帧和音频数据不必塞入普通控制事件队列。

SDK 的 JSON 工具只负责存储便利性，不替插件决定业务配置；敏感配置不得通过
日志、事件广播或查询命令返回。

## 代码归属

源码分为三种归属：`sdk/` 保存公开事件/契约/插件 API，`platform/` 保存进程与控制面板宿主，
`src/plugins/<name>/` 保存领域实现。模型、窗口、输入及 GL 生命周期强耦合，统一属于
desktop 插件；图像/OCR/截图属于 perception，Unix socket 与 GUI 客户端属于 IPC。
语音和剪贴板朗读分别属于 speech 与 reading_clip。

平台的构建与验证工具还包括根 `cmake/`、`scripts/`、`tests/`、`.gitea/`、安装脚本及
打包入口。根 `deskwaifu-ctl` 是 IPC 插件提供的公开命令行程序；`research/ocr/`
是 perception 插件的开发工具。第三方 Cubism、GLEW、stb 和推理库保持外部依赖身份。

`cmake/PluginBoundaries.cmake` 校验源文件、包含目录及传递链接依赖；C/C++ 编译器产生的
实际依赖还会检查绝对/相对路径的跨归属包含。生成的目标产物清单在链接前校验归属，
不依赖库的输出目录，包含 linker 转发参数和响应文件。已有编译/链接 launcher 保留。
静态归档器也校验实际对象输入；库搜索校验尊重静态/动态选择及其 push/pop 状态。
PCH 的 `REUSE_FROM` 同样校验提供目标归属，不能以预编译头绕过私有头依赖检查。
插件实现符号隐藏，模块禁止未解析符号。完整构建要求 CMake 3.25 及以上。
这约束工程依赖，不是对同进程受信任代码的安全沙箱。

SDK 仅保留公共值和通信契约。帧交换、命中测试、alpha 输入轮廓和缩放策略均为
desktop 私有实现；公共 `ImageFile` 只管理共享临时文件寿命，图像编码工厂归 perception。

平台只保留进程引导、Qt 事件循环、宿主配置、发现/生命周期、
通用事件路由及诊断。控制面板现已支持可选 GUI 入口，领域页面源码归入各自模块的
`gui/` 目录；详见 [GUI 插件归属](gui-plugins.md)。TAB 与后端处理器不必一一对应，
插件间只能通过 SDK 契约交互，不能编译、链接或包含其他插件的私有实现。

插件负责自己的配置语义。桌面插件保留既有主配置中的桌面设置，宿主只解释其中
`plugin_dir`；其他插件可在独立目录维护配置。语音插件不由交互层逐帧同步凭据。旧主配置中的
`minimax_api_key` 不会被自动导入插件；升级后应通过现有控制端重新设置凭据。
迁移不会主动删除旧文件中的密钥，用户应自行从旧配置中移除该字段。

## 构建与开发

各模块独立维护 `src/plugins/<name>/CMakeLists.txt`，产物统一为
`build/plugins/<name>/<name>.so`。开发配置例如：

```json
{"plugin_dir":"/绝对路径/DeskWaifu/build/plugins"}
```

主程序还可通过 `DESKWAIFU_PLUGIN_DIR` 显式覆盖发现目录，空值禁用发现。
`DESKWAIFU_BUILD_DESKTOP_PLUGIN`、`DESKWAIFU_BUILD_PERCEPTION_PLUGIN`、
`DESKWAIFU_BUILD_IPC_PLUGIN`、`DESKWAIFU_BUILD_SPEECH_PLUGIN`、
`DESKWAIFU_BUILD_READING_CLIP_PLUGIN` 分别控制后端模块；`DESKWAIFU_BUILD_CONTROL`
控制 GUI 宿主与 GUI 入口。全部业务模块关闭时，平台仍可独立构建、启动和测试。
完整 `DESKWAIFU_TEST_GATE` 要求所有上述能力启用，不允许用关闭模块缩减完整门禁。

`-DDESKWAIFU_BUILD_SPEECH_PLUGIN=OFF` 不构建语音模块，也不要求 Qt Multimedia
或 WebSockets；朗读插件仍可加载，但调用缺失的语音能力会明确失败。
默认的 `DeskWaifu` 安装组件包含共享事件/SDK运行库和已启用插件，保留现有安装方式；
此轮不改变 AUR 分包与依赖元数据。

可用 `DeskWaifuSDK` 安装组件安装头文件和 CMake package，外部模块使用
`find_package(DeskWaifuPluginSDK REQUIRED)` 并链接 `DeskWaifu::plugin_sdk`。
SDK 开发组件需要配套运行库；编译器、Qt 和 C++ 标准必须与宿主一致，ABI 检查不匹配
时拒绝加载。详细接口及独立测试见 [SDK 文档](../sdk/README.md)。
