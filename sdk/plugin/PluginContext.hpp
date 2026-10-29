#pragma once

#include "ConfigJson.hpp"
#include "event/ProcessorEventLoop.hpp"

class PluginManager;

// Owned by the manager, and alive from Initialize through Stop and destruction.
// Endpoints are owned here, so a failed initializer cannot leak registrations.
class PluginContext final
{
public:
    const QString& Name() const { return _name; }
    const QString& ConfigDirectory() const { return _configDirectory; }
    // Transitional adapters may reuse RAII Processor implementations on the
    // same loop. Their legacy registrations remain the adapter's responsibility.
    ProcessorEventLoop& Loop() const { return _loop; }
    EventEndpoint* CreateEndpoint(const QString& localName, EventEndpoint::Handler handler);
    // Loop-thread diagnostics remain available during Stop, after invalidation.
    void ReportFailure(const QString& error);
    bool ReadConfig(const QString& fileName, QJsonObject& object, QString* error = nullptr) const;
    bool WriteConfig(const QString& fileName, const QJsonObject& object, QString* error = nullptr) const;

private:
    friend class PluginManager;
    PluginContext(ProcessorEventLoop& loop, QString name, QString configDirectory);
    void Activate();
    void Invalidate();
    QString ConfigPath(const QString& fileName, QString* error) const;
    ProcessorEventLoop& _loop;
    QString _name, _configDirectory;
    std::vector<std::unique_ptr<EventEndpoint>> _endpoints;
    bool _active = false, _valid = true;
    QString _failure;
};
