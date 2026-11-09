#pragma once

#include "processor/Processor.hpp"
#include "LAppIPC.hpp"
#include <QSocketNotifier>
#include <memory>
#include <vector>
#include <map>

class IpcProcessor final : public Processor, public ICommandService
{
public:
    IpcProcessor(ProcessorEventLoop& loop, LAppIPC& ipc);
    IpcProcessor(ProcessorEventLoop& loop, std::unique_ptr<LAppIPC> ipc, IProcessorPlugin& commands);
    ~IpcProcessor() override;
    bool Start();
    void Stop() noexcept override;
    std::string Execute(const std::string&, const std::string&) override
    { return R"({"ok":false,"error":"use asynchronous command dispatch"})"; }
    void ExecuteAsync(const std::string& json, const std::string& connection, std::function<void(std::string)> complete) override;
    void Disconnected(const std::string& connection) override;
    bool HandleEvent(const ProcessorEvent& event) override;
    void SetPaused(bool paused);
private:
    void Schedule();
    void Refresh();
    struct Request { std::string json, connection; std::function<void(std::string)> complete; };
    void Dispatch(const Request& request);
    std::unique_ptr<LAppIPC> _owned;
    LAppIPC& _ipc;
    IProcessorPlugin* _commands = nullptr;
    std::map<std::string, Request> _requests;
    bool _stopped = false;
    bool _queued = false, _paused = false;
    std::vector<std::unique_ptr<QSocketNotifier>> _notifiers;
};
