#include "HostConfig.hpp"
#include "plugin/PluginManager.hpp"
#include "plugin/PluginCommand.hpp"
#include <QGuiApplication>
#include <cstdio>

int main(int argc, char* argv[])
{
    const int command = RunPluginCommand(argc, argv, QStringLiteral(DESKWAIFU_PLUGIN_DIR));
    if (command >= 0) return command;
    QGuiApplication application(argc, argv);
    application.setApplicationName("deskwaifu");
    application.setQuitOnLastWindowClosed(false);
    HostConfig config;
    QString error;
    if (!HostConfig::Load(application.arguments(), config, error)) {
        std::fprintf(stderr, "[Config] %s\n", qPrintable(error));
        return 1;
    }
    ProcessorEventLoop loop;
    PluginManager plugins(loop, config.pluginDirectory, config.configDirectory);
    if (!plugins.Start(&error)) std::fprintf(stderr, "[Plugins] %s\n", qPrintable(error));
    const int result = application.exec();
    if (!plugins.Stop(&error)) {
        std::fprintf(stderr, "[Plugins] %s\n", qPrintable(error));
        return 1;
    }
    return result;
}
