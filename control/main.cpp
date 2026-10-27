#include "MainWindow.hpp"

#include <QApplication>
#include <QLibraryInfo>
#include <QTimer>
#include <QTranslator>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("DeskWaifu Control"));
    application.setApplicationDisplayName(QStringLiteral("DeskWaifu 控制面板"));
    application.setOrganizationName(QStringLiteral("DeskWaifu"));

    QTranslator qtTranslator;
    if (qtTranslator.load(QStringLiteral("qtbase_zh_CN"), QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        application.installTranslator(&qtTranslator);

    MainWindow window;
    window.show();

    if (application.arguments().contains(QStringLiteral("--smoke-test")))
    {
        // Probe the application protocol, not the availability of a particular
        // GUI plugin. A connected but unresponsive socket must not pass smoke.
        auto& services = window.hostServices();
        QTimer::singleShot(15000, &application, [&application] { application.exit(1); });
        QObject::connect(&services, &GuiHostServices::responseReceived, &application,
            [&application](const QString& command, const QJsonObject& response) {
                if (command == QStringLiteral("get_status"))
                    application.exit(response.value(QStringLiteral("ok")).isBool() &&
                        response.value(QStringLiteral("ok")).toBool() ? 0 : 1);
            });
        QObject::connect(&services, &GuiHostServices::connectionChanged, &application,
            [&services](bool connected, const QString&) {
                if (connected) services.sendCommand(QStringLiteral("get_status"));
            });
        if (services.isConnected()) services.sendCommand(QStringLiteral("get_status"));
    }

    return application.exec();
}
