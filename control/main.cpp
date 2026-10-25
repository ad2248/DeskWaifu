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
        QTimer::singleShot(15000, &application, [&application] { application.exit(1); });
        QObject::connect(&window, &MainWindow::backendReady, &application,
            [&application] { application.exit(0); });
    }

    return application.exec();
}
