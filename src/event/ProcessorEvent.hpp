#pragma once

#include <QString>
#include <QVariantMap>

class IProcessor;

struct ProcessorEvent
{
    QString type;
    IProcessor* sender = nullptr;
    IProcessor* receiver = nullptr;
    QVariantMap data;
};

class IProcessor
{
public:
    virtual ~IProcessor() = default;
    virtual bool HandleEvent(const ProcessorEvent& event) = 0;
};
