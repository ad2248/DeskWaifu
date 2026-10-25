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
QString executablePath(const QString& applicationDirectory)
{
    const QString executable = QDir(applicationDirectory).absoluteFilePath(QStringLiteral("deskwaifu"));
    const QFileInfo localExecutable(executable);
    return localExecutable.isFile() && localExecutable.isExecutable()
        ? executable : QStandardPaths::findExecutable(QStringLiteral("deskwaifu"));
}

qint64 runningPid(const QString& procDirectory)
{
    const QDir processes(procDirectory);
    if (!processes.exists() || !QFileInfo(procDirectory).isReadable())
        return -1;

    for (const auto& entry : processes.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
    {
        bool numeric = false;
        const qint64 pid = entry.fileName().toLongLong(&numeric);
        if (!numeric || entry.ownerId() != getuid()) continue;

        QFile status(entry.filePath() + QStringLiteral("/status"));
        if (!status.open(QIODevice::ReadOnly)) continue; // 进程可能已经退出。
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
            return pid;
    }
    return 0;
}

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
    lock.setStaleLockTime(0); // 不能因启动缓慢而抢占仍在运行的启动器所持有的锁。
    if (!lock.tryLock(2000))
        return {State::Failed, QObject::tr("Cannot acquire the DeskWaifu startup lock; another launch may be in progress.")};

    // 当前用户的所有 GUI 共用此锁；获取锁后重新检查进程，
    // 并持锁直到 startDetached 完成 exec 成功握手。
    const qint64 running = runningPid(procDirectory);
    if (running < 0)
        return {State::Failed, QObject::tr("Cannot inspect running processes; DeskWaifu was not started.")};
    if (running > 0)
        return {State::AlreadyRunning,
                QObject::tr("DeskWaifu is already running (PID %1); trying to connect.").arg(running), running};

    const QString executable = executablePath(applicationDirectory);
    if (executable.isEmpty())
        return {State::Failed, QObject::tr("Cannot find deskwaifu next to the GUI or in PATH.")};

    QProcess process;
    process.setProgram(executable);
    process.setWorkingDirectory(QFileInfo(executable).absolutePath());
    process.setStandardInputFile(QProcess::nullDevice());
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    qint64 pid = 0;
    // 分离会话独立于 GUI，GUI 销毁或退出后仍继续运行。
    if (!process.startDetached(&pid))
        return {State::Failed, QObject::tr("Failed to start DeskWaifu: %1").arg(process.errorString())};
    // Linux 上 startDetached 会等待 exec 成功，因此 RAII 锁释放前，
    // /proc 已能提供可执行文件和进程名，无需等待套接字。
    return {State::Started, QObject::tr("Started DeskWaifu (PID %1); waiting for connection.").arg(pid), pid};
}
}
