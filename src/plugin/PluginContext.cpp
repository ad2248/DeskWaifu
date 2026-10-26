#include "PluginContext.hpp"
#include <QDir>
#include <QFileInfo>

PluginContext::PluginContext(ProcessorEventLoop& loop, QString name, QString configDirectory)
    : _loop(loop), _name(std::move(name)), _configDirectory(std::move(configDirectory)) {}

EventEndpoint* PluginContext::CreateEndpoint(const QString& localName, EventEndpoint::Handler handler)
{
    if (!_valid || localName.isEmpty() || localName.contains('/')) return nullptr;
    auto endpoint = _loop.CreateEndpoint(_name + '/' + localName, std::move(handler), _active);
    if (!endpoint) return nullptr;
    auto* result = endpoint.get();
    _endpoints.push_back(std::move(endpoint));
    return result;
}

void PluginContext::Activate()
{
    _active = true;
    for (const auto& endpoint : _endpoints) endpoint->Activate();
}

void PluginContext::Invalidate()
{
    _valid = false;
    _active = false;
    for (const auto& endpoint : _endpoints) endpoint->Invalidate();
}

QString PluginContext::ConfigPath(const QString& fileName, QString* error) const
{
    if (fileName.isEmpty() || fileName == "." || fileName == ".." || QFileInfo(fileName).fileName() != fileName) {
        if (error) *error = QStringLiteral("Configuration name must be a single file name");
        return {};
    }
    return QDir(_configDirectory).filePath(fileName);
}

bool PluginContext::ReadConfig(const QString& fileName, QJsonObject& object, QString* error) const
{
    const auto path = ConfigPath(fileName, error);
    return !path.isEmpty() && ReadJsonObject(path, object, error);
}

bool PluginContext::WriteConfig(const QString& fileName, const QJsonObject& object, QString* error) const
{
    const auto path = ConfigPath(fileName, error);
    return !path.isEmpty() && WriteJsonObject(path, object, error);
}
