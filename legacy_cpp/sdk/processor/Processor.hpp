// 提供借用事件循环的旧式处理器注册基类。
#pragma once

#include "event/ProcessorEventLoop.hpp"

// 为旧式处理器提供自动注册与发送辅助；不拥有事件循环。
// 在循环所属线程构造、Stop 和析构，且 loop 必须比处理器存活更久。
class Processor : public QObject, public IProcessorPlugin {
public:
    explicit Processor(ProcessorEventLoop& loop)
        : _loop(loop)
    {
        _loop.Register(this);
    }
    ~Processor() override { _loop.Unregister(this); }
    // 默认无需初始化，返回 true；派生类可覆盖以保存上下文或登记资源。
    bool Initialize(PluginContext& context, QString* error) override
    {
        Q_UNUSED(context);
        Q_UNUSED(error);
        return true;
    }
    // 撤销旧式注册，可重复调用；派生类仍需停止并等待自身工作线程。
    void Stop() noexcept override { _loop.Unregister(this); }

protected:
    // 以本对象为发送者投递事件；返回是否入队，不表示已处理。
    // receiver 为空时顺序广播，否则定向投递；非空接收者必须已注册。
    bool Send(const QString& type, IProcessorPlugin* receiver = nullptr, QVariantMap data = { }) { return _loop.Post({ type, this, receiver, std::move(data) }); }
    ProcessorEventLoop& _loop;
};
