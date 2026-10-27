#include "HostConfig.hpp"
#include "plugin/PluginManager.hpp"
#include <QGuiApplication>
#include <cstdio>

int main(int argc, char* argv[])
{
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
