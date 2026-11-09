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
    // 在 GUI 线程安装 SIGUSR1/SIGUSR2 监听；进程内只允许一个活动实例。
    explicit SignalProcessor(ProcessorEventLoop& loop);
    // 恢复原信号处理器，等待正在执行的处理函数退出后关闭管道。
    ~SignalProcessor() override;
    // 管道和两个信号处理器均安装成功时返回 true。
    bool Available() const { return bool(_notifier); }
    // 本处理器只由管道就绪触发事件发送，不消费入站 ProcessorEvent，始终返回 false。
    bool HandleEvent(const ProcessorEvent& event) override;
private:
    int _read = -1, _write = -1;
    struct sigaction _oldToggle{}, _oldFocus{};
    std::unique_ptr<QSocketNotifier> _notifier;
};
