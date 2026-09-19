#include "MainWindow.hpp"

#include <QApplication>
#include <QTimer>

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    application.setApplicationName(QStringLiteral("Waifuland Control"));
    application.setOrganizationName(QStringLiteral("Waifuland"));

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
