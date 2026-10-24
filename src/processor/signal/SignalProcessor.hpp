#pragma once
#include "processor/Processor.hpp"
#include <QSocketNotifier>
#include <csignal>
#include <functional>
#include <memory>

// POSIX handlers only write a byte to a nonblocking self-pipe. Business actions
// execute as ordinary GUI-owned ProcessorEvents, independently of presentation.
class SignalProcessor final : public Processor
{
public:
    SignalProcessor(ProcessorEventLoop& loop, std::function<void()> toggle, std::function<void()> focus);
    ~SignalProcessor() override;
    bool Available() const { return bool(_notifier); }
    bool HandleEvent(const ProcessorEvent& event) override;
private:
    int _read = -1, _write = -1;
    struct sigaction _oldToggle{}, _oldFocus{};
    std::unique_ptr<QSocketNotifier> _notifier;
    std::function<void()> _toggle, _focus;
};
