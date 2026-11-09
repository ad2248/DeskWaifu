#pragma once
#include "event/ProcessorEventLoop.hpp"
#include <QStringList>
#include <memory>

class LoadedBackendPlugins
{
public:
    LoadedBackendPlugins(ProcessorEventLoop& loop, const QStringList& modules);
    ~LoadedBackendPlugins();
    void Stop();
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
