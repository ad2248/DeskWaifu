#include <QCoreApplication>
#include <QTimer>

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    // Safety net if the test is interrupted before its cleanup runs.
    QTimer::singleShot(30000, &application, &QCoreApplication::quit);
    return application.exec();
}
