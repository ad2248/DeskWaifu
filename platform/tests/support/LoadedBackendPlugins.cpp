#include "LoadedBackendPlugins.hpp"
#include "plugin/PluginManager.hpp"
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <stdexcept>

struct LoadedBackendPlugins::Impl
{
    QTemporaryDir installation;
    std::unique_ptr<PluginManager> plugins;
};
LoadedBackendPlugins::LoadedBackendPlugins(ProcessorEventLoop& loop, const QStringList& modules) : _impl(new Impl)
{
    if (!_impl->installation.isValid()) throw std::runtime_error("private backend plugin installation");
    int index = 0;
    for (const auto& module : modules) {
        const auto name = QString::number(index++);
        const auto destination = _impl->installation.path() + '/' + name;
        if (!QDir().mkpath(destination) || !QFile::copy(module, destination + '/' + name + ".so"))
            throw std::runtime_error("install backend test module");
    }
    _impl->plugins.reset(new PluginManager(loop, _impl->installation.path(), _impl->installation.path()));
    QString error;
    if (!_impl->plugins->Start(&error)) throw std::runtime_error(error.toStdString());
    if (_impl->plugins->LoadedPlugins().size() != modules.size())
        throw std::runtime_error("not all requested backend modules were loaded");
}
LoadedBackendPlugins::~LoadedBackendPlugins() = default;
void LoadedBackendPlugins::Stop() { _impl->plugins->Stop(); }
