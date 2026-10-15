#include "DeskWaifuLauncher.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QObject>
#include <QProcess>
#include <QStandardPaths>
#include <unistd.h>

namespace DeskWaifuLauncher
{
Result spawn(bool connected, const QString& applicationDirectory, const QString& procDirectory,
             const QString& lockDirectory)
{
    if (connected)
        return {State::Connected, QObject::tr("DeskWaifu is already connected.")};

    const QString runtime = lockDirectory.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) : lockDirectory;
    if (runtime.isEmpty())
        return {State::Failed, QObject::tr("Cannot find a runtime directory for the DeskWaifu startup lock.")};
    QLockFile lock(QDir(runtime).filePath(QStringLiteral("deskwaifu-spawn.lock")));
    lock.setStaleLockTime(0); // Never steal a live launcher's lock because startup is slow.
    if (!lock.tryLock(2000))
        return {State::Failed, QObject::tr("Cannot acquire the DeskWaifu startup lock; another launch may be in progress.")};

    // Every GUI for this user shares the lock. Recheck only after acquiring it,
    // and keep it held across startDetached's successful exec handshake.
    const QDir processes(procDirectory);
    if (!processes.exists() || !QFileInfo(procDirectory).isReadable())
        return {State::Failed, QObject::tr("Cannot inspect running processes; DeskWaifu was not started.")};

    for (const auto& entry : processes.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
    {
        bool numeric = false;
        const qint64 pid = entry.fileName().toLongLong(&numeric);
        if (!numeric || entry.ownerId() != getuid()) continue;

        QFile status(entry.filePath() + QStringLiteral("/status"));
        if (!status.open(QIODevice::ReadOnly)) continue; // Process may have exited.
        const QByteArray contents = status.readAll();
        bool namedDeskWaifu = false;
        bool zombie = false;
        for (const auto& line : contents.split('\n'))
        {
            if (line.startsWith("Name:")) namedDeskWaifu = line.mid(5).trimmed() == "deskwaifu";
            if (line.startsWith("State:")) zombie = line.mid(6).trimmed().startsWith('Z');
        }
        const QString executable = QFileInfo(entry.filePath() + QStringLiteral("/exe")).symLinkTarget();
        if (!zombie && (namedDeskWaifu || QFileInfo(executable).fileName() == QStringLiteral("deskwaifu")))
            return {State::AlreadyRunning,
                    QObject::tr("DeskWaifu is already running (PID %1); trying to connect.").arg(pid), pid};
    }

    QString executable = QDir(applicationDirectory).absoluteFilePath(QStringLiteral("deskwaifu"));
    const QFileInfo localExecutable(executable);
    if (!localExecutable.isFile() || !localExecutable.isExecutable())
        executable = QStandardPaths::findExecutable(QStringLiteral("deskwaifu"));
    if (executable.isEmpty())
        return {State::Failed, QObject::tr("Cannot find deskwaifu next to the GUI or in PATH.")};

    QProcess process;
    process.setProgram(executable);
    process.setWorkingDirectory(QFileInfo(executable).absolutePath());
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    qint64 pid = 0;
    // A detached session is independent of the GUI and survives its destruction/exit.
    if (!process.startDetached(&pid))
        return {State::Failed, QObject::tr("Failed to start DeskWaifu: %1").arg(process.errorString())};
    // On Linux startDetached waits for exec to succeed, so /proc already exposes
    // the executable/name before the RAII lock is released (no socket wait needed).
    return {State::Started, QObject::tr("Started DeskWaifu (PID %1); waiting for connection.").arg(pid), pid};
}
}
