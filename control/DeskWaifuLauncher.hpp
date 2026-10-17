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
// Directories are injectable to isolate process fixtures and cross-process locks in tests.
Result spawn(bool connected, const QString& applicationDirectory,
             const QString& procDirectory = QStringLiteral("/proc"),
             const QString& lockDirectory = {});
}
