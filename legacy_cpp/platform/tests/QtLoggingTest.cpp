#include "../QtLogging.hpp"
#include "plugin/GuiLog.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <iostream>

#define CHECK(condition)                                                                                                                                                           \
    do {                                                                                                                                                                           \
        if (!(condition)) {                                                                                                                                                        \
            std::cerr << __LINE__ << ": " << #condition << '\n';                                                                                                                   \
            return 1;                                                                                                                                                              \
        }                                                                                                                                                                          \
    } while (false)

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    qunsetenv("DESKWAIFU_LOG_DIR");
    const auto path = InitializeQtLogging(QStringLiteral("test.log"));
    CHECK(path.startsWith(QDir::tempPath() + QStringLiteral("/log/waifudesk_")));
    CHECK(QFileInfo::exists(path));
    const auto safe = FormatGuiResponseLog(QStringLiteral("set_speech_settings"),
        { { QStringLiteral("ok"), false }, { QStringLiteral("api_key"), QStringLiteral("secret") }, { QStringLiteral("error"), QStringLiteral("secret") } });
    CHECK(safe.contains(QStringLiteral("set_speech_settings")));
    CHECK(safe.contains(QStringLiteral("failure")));
    CHECK(!safe.contains(QStringLiteral("secret")));
    const auto allowed = FormatGuiResponseLog(QStringLiteral("set_render_device"),
        { { QStringLiteral("ok"), true }, { QStringLiteral("restart_required"), true }, { QStringLiteral("unexpected"), QStringLiteral("secret") } });
    CHECK(allowed.contains(QStringLiteral("restart_required")));
    CHECK(!allowed.contains(QStringLiteral("unexpected")));
    qInfo().noquote() << safe;
    qInfo().noquote() << "first message";
    QFile first(path);
    CHECK(first.open(QIODevice::ReadOnly));
    const auto firstContents = first.readAll();
    CHECK(firstContents.contains("set_speech_settings"));
    CHECK(firstContents.contains("failure"));
    CHECK(firstContents.contains("first message"));
    CHECK(!firstContents.contains("secret"));
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
