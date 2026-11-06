#include "HostConfig.hpp"
#include "QtLogging.hpp"
#include "plugin/PluginManager.hpp"
#include "plugin/PluginCommand.hpp"
#include <QDebug>
#include <QGuiApplication>

int main(int argc, char* argv[])
{
    InitializeQtLogging(QStringLiteral("daemon.log"));
    const int command = RunPluginCommand(argc, argv, QStringLiteral(DESKWAIFU_PLUGIN_DIR));
    if (command >= 0) return command;
    QGuiApplication application(argc, argv);
    application.setApplicationName("deskwaifu");
    application.setQuitOnLastWindowClosed(false);
    HostConfig config;
    QString error;
    if (!HostConfig::Load(application.arguments(), config, error)) {
        qCritical() << "config" << "failure";
        return 1;
    }
    ProcessorEventLoop loop;
    PluginManager plugins(loop, config.pluginDirectory, config.configDirectory);
    if (!plugins.Start(&error)) qCritical() << "plugins" << "failure";
    const int result = application.exec();
    if (!plugins.Stop(&error)) {
        qCritical() << "plugins" << "shutdown_failure";
        return 1;
    }
    return result;
}
