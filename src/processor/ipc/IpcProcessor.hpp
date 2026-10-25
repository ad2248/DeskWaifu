#pragma once

#include "processor/Processor.hpp"
#include "LAppIPC.hpp"
#include <QSocketNotifier>
#include <memory>
#include <vector>

class IpcProcessor final : public Processor
{
public:
    IpcProcessor(ProcessorEventLoop& loop, LAppIPC& ipc);
    bool HandleEvent(const ProcessorEvent& event) override;
    void SetPaused(bool paused);
private:
    void Schedule();
    void Refresh();
    LAppIPC& _ipc;
    bool _queued = false, _paused = false;
    std::vector<std::unique_ptr<QSocketNotifier>> _notifiers;
};
