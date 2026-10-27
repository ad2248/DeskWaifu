#pragma once

#include "event/ProcessorEventLoop.hpp"

class Processor : public QObject, public IProcessorPlugin
{
public:
    explicit Processor(ProcessorEventLoop& loop) : _loop(loop) { _loop.Register(this); }
    ~Processor() override { _loop.Unregister(this); }
    bool Initialize(PluginContext& context, QString* error) override
    { Q_UNUSED(context); Q_UNUSED(error); return true; }
    void Stop() noexcept override { _loop.Unregister(this); }

protected:
    bool Send(const QString& type, IProcessorPlugin* receiver = nullptr, QVariantMap data = {})
    { return _loop.Post({type, this, receiver, std::move(data)}); }
    ProcessorEventLoop& _loop;
};
