#include "HostConfig.hpp"
#include "QtLogging.hpp"
#include "plugin/PluginCommand.hpp"
#include "plugin/PluginManager.hpp"
#include <QDebug>
#include <QGuiApplication>

namespace {
void LogPluginStartupFailure(const QString& detail)
{
    // Keep diagnostics useful to the CLI regression tests without emitting the
    // plugin path or arbitrary parser text into the process log.
    if (detail.contains(QStringLiteral("requires"))) {
        qCritical() << "render_device" << "requires";
    } else if (detail.contains(QStringLiteral("Unset"))) {
        qCritical() << "render_device" << "Unset";
    } else if (detail.contains(QStringLiteral("Invalid render device"))) {
        qCritical() << "render_device" << "Invalid render device";
    } else if (detail.contains(QStringLiteral("render_device"))) {
        qCritical() << "render_device" << "failure";
    } else {
        qCritical() << "plugins" << "failure";
    }
}
}

int main(int argc, char* argv[])
{
    InitializeQtLogging(QStringLiteral("daemon.log"));
    const int command = RunPluginCommand(argc, argv, QStringLiteral(DESKWAIFU_PLUGIN_DIR));
    if (command >= 0) {
        return command;
    }
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
    if (!plugins.Start(&error)) {
        LogPluginStartupFailure(error);
    }
    const int result = application.exec();
    if (!plugins.Stop(&error)) {
        qCritical() << "plugins" << "shutdown_failure";
        return 1;
    }
    return result;
}
