#pragma once
#include "processor/Processor.hpp"
#include <QSocketNotifier>
#include <csignal>
#include <memory>

// POSIX 信号处理函数只向非阻塞自管道写入一个字节。
// 业务操作作为 GUI 线程中的普通 ProcessorEvent 执行，不依赖呈现。
class SignalProcessor final : public Processor
{
public:
    explicit SignalProcessor(ProcessorEventLoop& loop);
    ~SignalProcessor() override;
    bool Available() const { return bool(_notifier); }
    bool HandleEvent(const ProcessorEvent& event) override;
private:
    int _read = -1, _write = -1;
    struct sigaction _oldToggle{}, _oldFocus{};
    std::unique_ptr<QSocketNotifier> _notifier;
};
