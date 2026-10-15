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

// procDirectory is injectable so tests never inspect or start the user's backend.
Result spawn(bool connected, const QString& applicationDirectory,
             const QString& procDirectory = QStringLiteral("/proc"));
}
