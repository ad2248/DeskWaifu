#pragma once

#include "event/ProcessorEventLoop.hpp"

class Processor : public QObject, public IProcessor
{
public:
    explicit Processor(ProcessorEventLoop& loop) : _loop(loop) { _loop.Register(this); }
    ~Processor() override { _loop.Unregister(this); }

protected:
    bool Send(const QString& type, IProcessor* receiver = nullptr, QVariantMap data = {})
    { return _loop.Post({type, this, receiver, std::move(data)}); }
    ProcessorEventLoop& _loop;
};
