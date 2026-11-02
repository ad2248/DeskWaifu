#pragma once

#include <QString>

namespace DeskWaifuLauncher
{
enum class State { Connected, AlreadyRunning, Started, Failed };

struct Result
{
    State state;
    QString message;
    qint64 pid = 0;
};

QString executablePath(const QString& applicationDirectory);
// 仅检查同 UID、同 XDG_RUNTIME_DIR 的后端；不存在时返回 0，无法检查时返回 -1。
qint64 runningPid(const QString& procDirectory = QStringLiteral("/proc"));
// 允许注入目录，以隔离测试中的进程夹具和跨进程锁。
Result spawn(bool connected, const QString& applicationDirectory,
             const QString& procDirectory = QStringLiteral("/proc"),
             const QString& lockDirectory = {});
}
