#pragma once

#include "plugin/PluginApi.hpp"
#include "plugin/PluginContext.hpp"
#include <QStringList>

// Loop-thread-only. Failed plugins are rolled back individually; healthy ones
// remain available. Start returns false with diagnostics on partial failure.
class PluginManager final
{
public:
    PluginManager(ProcessorEventLoop& loop, QString pluginRoot, QString configRoot);
    ~PluginManager();
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;
    bool Start(QString* error = nullptr);
    bool Stop(QString* error = nullptr);
    QStringList LoadedPlugins() const;
    static QStringList Discover(const QString& root); // Exact <root>/<name>/<name>.so, no recursion.

private:
    struct Instance
    {
        QString name;
        const PluginApi* api = nullptr;
        IProcessorPlugin* plugin = nullptr;
        std::unique_ptr<PluginContext> context;
    };
    ProcessorEventLoop& _loop;
    QString _pluginRoot, _configRoot;
    std::vector<Instance> _instances;
    bool _started = false;
};
