#pragma once

#include <QString>
#include <QVariantMap>
#include <memory>
#include <utility>

class IProcessor;

struct ProcessorEvent
{
    ProcessorEvent(QString type = {}, IProcessor* sender = nullptr,
                   IProcessor* receiver = nullptr, QVariantMap data = {})
        : type(std::move(type)), sender(sender), receiver(receiver), data(std::move(data)) {}
    virtual ~ProcessorEvent() = default;

    QString type;
    IProcessor* sender = nullptr;
    IProcessor* receiver = nullptr;
    QVariantMap data;
};

// Share the dynamic event, never copy a base reference into a queue.
using ProcessorEventPtr = std::shared_ptr<const ProcessorEvent>;

class IProcessor
{
public:
    virtual ~IProcessor() = default;
    virtual bool HandleEvent(const ProcessorEvent& event) = 0;
};
