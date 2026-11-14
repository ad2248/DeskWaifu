#pragma once

#include "LAppIPC.hpp"
#include "processor/Processor.hpp"
#include <QSocketNotifier>
#include <map>
#include <memory>
#include <vector>

// 在事件循环线程把套接字就绪通知转换为异步命令派发，不依赖渲染轮询。
class IpcProcessor final : public Processor, public ICommandService {
public:
    // 借用已配置的传输对象；ipc 必须比处理器存活更久。
    IpcProcessor(ProcessorEventLoop& loop, LAppIPC& ipc);
    // 接管传输对象所有权；commands 是异步命令接收方，需保持注册且存活。
    IpcProcessor(ProcessorEventLoop& loop, std::unique_ptr<LAppIPC> ipc, IProcessorPlugin& commands);
    ~IpcProcessor() override;
    // 初始化监听及就绪通知；已停止或初始化失败时返回 false。
    bool Start();
    // 幂等关闭传输，并以停止错误完成尚未回复的请求。
    void Stop() noexcept override;
    // 同步执行不受支持，始终返回错误 JSON。
    std::string Execute(const std::string&, const std::string&) override { return R"({"ok":false,"error":"use asynchronous command dispatch"})"; }
    // 每个 connection 同时至多有一个请求；结果到达时调用 complete。
    void ExecuteAsync(const std::string& json, const std::string& connection, std::function<void(std::string)> complete) override;
    // 丢弃该连接的待回调请求，并广播会话断开事件。
    void Disconnected(const std::string& connection) override;
    bool HandleEvent(const ProcessorEvent& event) override;
    // 暂停就绪处理；恢复时重新调度，保留未完成的请求。
    void SetPaused(bool paused);

private:
    void Schedule();
    void Refresh();
    struct Request {
        std::string json, connection;
        std::function<void(std::string)> complete;
    };
    void Dispatch(const Request& request);
    std::unique_ptr<LAppIPC> _owned;
    LAppIPC& _ipc;
    IProcessorPlugin* _commands = nullptr;
    std::map<std::string, Request> _requests;
    bool _stopped = false;
    bool _queued = false, _paused = false;
    std::vector<std::unique_ptr<QSocketNotifier>> _notifiers;
};
