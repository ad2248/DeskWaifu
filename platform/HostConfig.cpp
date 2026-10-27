#include "HostConfig.hpp"
#include "contracts/ConfigStore.hpp"
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

bool HostConfig::Load(const QStringList& arguments, HostConfig& config, QString& error)
{
    QString path = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/deskwaifu/config.json";
    const int index = arguments.indexOf("--config");
    if (index >= 0) {
        if (index + 1 == arguments.size()) { error = "--config requires a path"; return false; }
        path = arguments[index + 1];
    }
    if (path.startsWith("~/")) path = QDir::homePath() + path.mid(1);
    config.configDirectory = QFileInfo(path).absolutePath();
    config.pluginDirectory = QStringLiteral(DESKWAIFU_PLUGIN_DIR);
    ConfigStore::Snapshot snapshot;
    std::string diagnostic;
    if (!ConfigStore(path.toStdString()).Read(snapshot, diagnostic) && !diagnostic.empty()) {
        error = QString::fromStdString(diagnostic); return false;
    }
    const auto value = snapshot.values.find("plugin_dir");
    if (value != snapshot.values.end()) {
        std::string directory;
        if (!ConfigStore::ParseString(value->second, directory)) { error = "plugin_dir must be a string"; return false; }
        config.pluginDirectory = QString::fromStdString(directory);
        if (config.pluginDirectory.startsWith("~/")) config.pluginDirectory = QDir::homePath() + config.pluginDirectory.mid(1);
    }
    if (qEnvironmentVariableIsSet("DESKWAIFU_PLUGIN_DIR"))
        config.pluginDirectory = qEnvironmentVariable("DESKWAIFU_PLUGIN_DIR");
    return true;
}
