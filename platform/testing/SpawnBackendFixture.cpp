#include <QCoreApplication>
#include <QTimer>

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    // 测试在执行清理前被中断时的安全兜底。
    QTimer::singleShot(30000, &application, &QCoreApplication::quit);
    return application.exec();
}
