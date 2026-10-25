# 插件 SDK（ABI v1）

宿主只认识插件生命周期和消息，不认识语音、OCR、reading 等领域。没有 manifest、依赖解析、市场或热重载；**同进程插件不是崩溃隔离边界**。

## 宿主接线

本目录可独立验证，不需要 Cubism：

```sh
cmake -S src/plugin -B /tmp/opencode/plugin-sdk-build -DCMAKE_CXX_STANDARD=14
cmake --build /tmp/opencode/plugin-sdk-build -j4
ctest --test-dir /tmp/opencode/plugin-sdk-build --output-on-failure
```

在应用构建中：

1. 将现有 `deskwaifu_events` 目标由 `STATIC` 改为 **`SHARED`**，推荐设置 `VERSION 1.0 SOVERSION 1`。宿主和所有模块必须共享它，不能各自静态嵌入事件实现。
2. 在定义 `deskwaifu_events` 后 `add_subdirectory(plugin)`，应用链接 `deskwaifu_plugin_host`。插件只链接 `deskwaifu_plugin_sdk`（会传递依赖 `deskwaifu_events` / `Qt6::Core`），不链接整个应用。
3. 随安装打包 `libdeskwaifu_events.so.1` 和 `libdeskwaifu_plugin_sdk.so.1` 并配置动态库搜索路径。独立构建的 CMake 不替应用规定安装目录。
4. 与现有处理器共用一个 `ProcessorEventLoop`；主线程创建 `PluginManager(loop, pluginRoot, configRoot)`，调用 `Start(&error)`。退出时先 `Stop()`（停止并 join 工作线程），之后再销毁 loop。处理器可使用相同的端点 API，不必成为动态插件。

`Discover(root)` 只接受 `<root>/<name>/<name>.so` 普通文件，按目录名排序；不接受 `lib<name>.so`、平铺模块、递归目录、目录/模块符号链接。缺少根目录表示没有可选插件。`LoadedPlugins()` 给出实例名。

`Start` 依次创建并 Initialize 各实例，随后激活成功初始化的端点，最后逐个 Start。初始化阶段禁止发送消息。某个入口/ABI/工厂/初始化/Start 失败，只失效并 Stop/destroy 失败实例，其他健康插件继续工作。返回 false 和汇总诊断表示存在失败项，不表示健康插件已停止；调用者仍须 Stop。**Stop 必须支持部分初始化，并且 noexcept。** 配置文件是持久数据，不随失败回滚删除。启动完成后再次 Start 会报错；Stop 幂等，Stop 后允许重新创建实例，但不会卸载或重新载入库代码。插件之间按运行时能力协作，首版不解析硬依赖图；依赖不可用由请求结果报告。

库设置 `QLibrary::PreventUnloadHint`，即便入口不兼容、启动失败或 Stop，也保持映射到进程退出。这样插件事件、Qt 元类型、延迟释放对象仍可执行其代码；不是热卸载方案。描述符在工厂调用前检查结构大小、SDK 版本、精确 Qt 版本、平台 ABI、编译器、C++ 标准及标准库 ABI。SDK 接口布局改变时必须提升 `DeskWaifuPluginAbiVersion` 和共享库 SOVERSION；所有组件需以相同工具链/Qt/标准构建。这些检查不能使恶意或损坏的 `.so` 变安全。

## 最小插件

领域契约应放在宿主和消费者都能包含的普通头文件，不放进通用总线。事件继承 `ProcessorEvent`，允许直接包含强类型字段，不必使用 QVariantMap：

```cpp
struct RenderText : ProcessorEvent { QString text; };
struct RenderResult : ProcessorEvent { bool ok; QString error; };

class TextPlugin final : public IPlugin {
    EventEndpoint* endpoint = nullptr; // 由 context 所有，Initialize 至 destroy 期间有效
public:
    bool Initialize(PluginContext& context, QString*) override {
        endpoint = context.CreateEndpoint("render", [this](const EventDelivery& d) {
            if (d.kind == EventDelivery::Kind::Cancellation) {
                // 以 d.requestId 定位并取消实际异步工作；不再 Respond。
                return;
            }
            if (d.kind != EventDelivery::Kind::Request) return;
            auto request = std::dynamic_pointer_cast<const RenderText>(d.payload);
            auto result = std::make_shared<RenderResult>();
            result->ok = bool(request);
            if (!request) result->error = "unsupported request";
            endpoint->Respond(d.requestId, result);
        });
        return endpoint && endpoint->Provide("text.render/v1");
    }
    void Stop() noexcept override { /* 取消实际工作并 join 线程，不能再使用 endpoint 发消息 */ }
};
DESKWAIFU_EXPORT_PLUGIN(TextPlugin)
```

模块 CMake 形状：

```cmake
add_library(text MODULE TextPlugin.cpp)
target_link_libraries(text PRIVATE deskwaifu_plugin_sdk)
set_target_properties(text PROPERTIES PREFIX "" OUTPUT_NAME text)
# 安装/输出到插件根目录下的 text/text.so
```

`PluginContext::Name()` 是目录名；`ConfigDirectory()` 是 `<configRoot>/plugin/<name>`。`ReadConfig("settings.json", object, &error)` / `WriteConfig(...)` 只接受单一文件名，写入自动创建目录。缺失/非法/非对象 JSON 返回 false，读取失败不改变输出；调用者决定默认值。`ConfigJson.hpp` 还提供按完整路径读写的 `ReadJsonObject` / `WriteJsonObject`。写入使用 `QSaveFile` 原子提交，禁止退回直接覆盖，不会留下半截 JSON。它不提供并发读改写事务。

渐进迁移适配器可通过 `context.Loop()` 复用原有 RAII `Processor`，不能另建第二条总线。Legacy 注册仍由该对象析构时解绑，工作线程由插件 Stop 收尾；Legacy 接口不受 context 的端点激活门控，因此插件必须遵守 Initialize 不发送、Start 才启动工作的约定。新能力和订阅应使用 context 所有的端点。

## 通知与能力请求

`EventEndpoint` 的处理函数是 `void(const EventDelivery&)`，不承担 Legacy 的 bool 消费规则。一个实例可用多个具名端点；context 自动加 `<pluginName>/` 前缀并拥有端点。重复名称、重复订阅、同一端点重复 Provide 返回失败。端点创建/订阅/提供/激活/失效只能在 loop 所在线程操作；创建或注册在错误线程明确失败。工作线程可在端点仍存活期间调用 Notify/Request/Respond/Cancel。Stop 必须 join 工作线程后才允许 context 析构。

```cpp
auto consumer = loop.CreateEndpoint("reading", [](const EventDelivery& d) {
    if (d.kind == EventDelivery::Kind::Notification) { /* 每个订阅者都可见 */ }
});
consumer->Subscribe("text.changed/v1");
consumer->Notify("text.changed/v1", std::make_shared<RenderText>());

auto request = std::make_shared<RenderText>();
request->text = "hello";
RequestId id = consumer->Request("text.render/v1", request,
    [](const RequestResult& r) {
        // r.requestId 是调用返回的关联 ID，回调始终异步在主线程执行。
        if (r.status != RequestStatus::Replied) { /* 传输/生命周期失败 */ return; }
        auto result = std::dynamic_pointer_cast<const RenderResult>(r.payload);
        // 领域失败由 RenderResult 表达；总线不理解领域错误。
    }, 30000);
consumer->Cancel(id);
```

* Topic / capability 都是显式字符串；领域自行使用 `/v1` 等版本后缀。通知发送时对当前活跃订阅者取快照，各自收到通知；任一处理者不能消费别人的通知。未知 topic 是无副作用的成功发送。
* 请求只匹配**恰好一个**活跃能力提供者；零个/多个分别异步返回 `NoProvider` / `AmbiguousProvider`，绝不按注册顺序选。未发送、空参数、无回调、超时非正数或发送端无效返回 ID 0；其余调用返回非零 ID。
* 提供者从 delivery 获取请求 ID，用自己的端点 `Respond(id, payload)`；只有匹配提供者可以响应，只有请求者可以 Cancel。成功响应为 `Replied`；领域成功、失败、拒绝由响应类型描述。未知事件类型应由提供者回一个领域级“不支持”，或者等待超时；总线不硬编码领域失败。
* `Cancelled`、`TimedOut`、`EndpointUnavailable`、`HandlerFailed` 都是终态。取消/超时/请求者失效会向仍活跃提供者投递 `Cancellation`（相同 ID、空 payload），提供者须自行终止实际异步工作。处理函数抛出的 C++ 异常会记录警告，请求返回 `HandlerFailed`；异常并非进程崩溃隔离。
* 响应、取消、超时以互斥锁内首次确立的终态为准；响应入口也检查单调时钟截止时间，不依赖主线程及时处理定时器。已结束/迟到/重复/错误端点的响应和取消返回 false，不再触发回调。终态已排队的响应不会被随后 Cancel 改写。
* 提供者失效使待决请求返回 `EndpointUnavailable`。请求者失效会取消其工作，**不再调用属于它的 completion**；已经排队的回调也跳过。同名新端点使用新 ID，不能接收或响应旧请求。通知/请求派发前发送者或接收者失效也跳过，防止已销毁插件回调。
* 除请求/响应正文外不要跨插件持有裸领域对象。共享事件投递后应视为不可变；生产者也不能继续修改它。所有旧、新消息共享一条 FIFO、非递归 Qt 主线程调度队列；嵌套 `processEvents()` 不会重入消息处理。外部 I/O 完成与超时仍要求 Qt 事件循环正常运转，没有实时保证。
* 新通知/请求的准入预算为 4096 条排队投递和 1024 个待决请求；饱和时 Notify 返回 false、Request 返回 0。通知按全部订阅者一次性检查预算，不部分发送。已接受请求的终态与取消不因预算耗尽被丢弃，可能暂时超出准入水位。Legacy Post 保留原有行为，不受此准入限制；这不是恶意插件的内存沙箱。

## Legacy 兼容

`Post({type, sender, receiver, data})`、`IProcessor::HandleEvent(const ProcessorEvent&)`、`Processor::Send` 保留，仍按注册顺序遇到 true 停止。新通知不受该规则影响。

`PostEvent(std::shared_ptr<const ProcessorEvent>)` 支持以基类共享所有权投递派生 Legacy 事件，`Post(ConcreteDerivedEvent)` 模板也保留动态类型。**不要把派生对象先复制成 `ProcessorEvent` 值，或调用 Legacy 按值 Post(baseReference)**；那是 C++ 本身的切片，无法恢复。跨线程/异步派生事件应始终采用 `make_shared<Derived>` 和基类共享指针接口。
