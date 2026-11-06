#include "../QtLogging.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) { std::cerr << __LINE__ << ": " << #condition << '\n'; return 1; } } while (false)

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    qunsetenv("DESKWAIFU_LOG_DIR");
    const auto path = InitializeQtLogging(QStringLiteral("test.log"));
    CHECK(path.startsWith(QDir::tempPath() + QStringLiteral("/log/waifudesk_")));
    CHECK(QFileInfo::exists(path));
    qInfo().noquote() << "first message";
    QFile first(path);
    CHECK(first.open(QIODevice::ReadOnly));
    CHECK(first.readAll().contains("first message"));
    first.close();

    InitializeQtLogging(QStringLiteral("test.log"));
    QFile truncated(path);
    CHECK(truncated.open(QIODevice::ReadOnly));
    CHECK(!truncated.readAll().contains("first message"));
    qInfo().noquote() << "second message";
    truncated.close();
    CHECK(truncated.open(QIODevice::ReadOnly));
    CHECK(truncated.readAll().contains("second message"));
    return 0;
}
