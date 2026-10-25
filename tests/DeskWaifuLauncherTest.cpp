#include "DeskWaifuLauncher.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>
#include <csignal>
#include <unistd.h>

using DeskWaifuLauncher::State;

class DeskWaifuLauncherTest : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir directory;
    QByteArray originalPath;
    bool hadPath = false;

    QString app() const { return directory.path() + "/app"; }
    QString proc() const { return directory.path() + "/proc"; }
    void process(const QString& pid, const QByteArray& name, const QByteArray& state = "S (sleeping)")
    {
        QVERIFY(QDir().mkpath(proc() + "/" + pid));
        QFile status(proc() + "/" + pid + "/status");
        QVERIFY(status.open(QIODevice::WriteOnly));
        status.write("Name:\t" + name + "\nState:\t" + state + "\n");
    }
    void installBackend(const QString& target)
    {
        QVERIFY(QDir().mkpath(target));
        QVERIFY(QFile::copy(QStringLiteral(SPAWN_BACKEND_FIXTURE), target + "/deskwaifu"));
    }
    static void stopBackend(qint64 pid)
    {
        if (pid <= 0) return;
        kill(pid, SIGKILL);
        const auto active = [pid] {
            QFile status(QStringLiteral("/proc/%1/status").arg(pid));
            return status.open(QIODevice::ReadOnly) && !status.readAll().contains("State:\tZ");
        };
        // 分离的夹具进程不是当前进程的子进程，应等待退出，而非调用 waitpid()。
        // 作用域清理可能在 QtTest 判定测试结束后执行；此时 QTRY 可能停止轮询，
        // 但仍必须确保分离进程退出。
        QVERIFY(QTest::qWaitFor([&] { return !active(); }, 2000));
    }

private slots:
    void init()
    {
        QVERIFY(directory.isValid());
        QVERIFY(QDir().mkpath(app()));
        QVERIFY(QDir().mkpath(proc()));
        hadPath = qEnvironmentVariableIsSet("PATH");
        originalPath = qgetenv("PATH");
        qputenv("PATH", app().toLocal8Bit());
    }
    void cleanup()
    {
        if (hadPath) qputenv("PATH", originalPath);
        else qunsetenv("PATH");
        QVERIFY(QDir(app()).removeRecursively());
        QVERIFY(QDir(proc()).removeRecursively());
        QDir(directory.path() + "/path").removeRecursively();
    }
    void connectedDoesNotInspectOrSpawn()
    {
        QCOMPARE(DeskWaifuLauncher::spawn(true, app(), "/nonexistent-proc").state, State::Connected);
    }
    void existingProcessDoesNotSpawn()
    {
        process("123", "deskwaifu");
        const auto result = DeskWaifuLauncher::spawn(false, app(), proc());
        QCOMPARE(result.state, State::AlreadyRunning);
        QCOMPARE(result.pid, 123);
    }
    void runningPidTracksProcessLifecycle()
    {
        QCOMPARE(DeskWaifuLauncher::runningPid(proc()), 0);
        process("123", "deskwaifu");
        QCOMPARE(DeskWaifuLauncher::runningPid(proc()), 123);
        process("123", "deskwaifu", "Z (zombie)");
        QCOMPARE(DeskWaifuLauncher::runningPid(proc()), 0);
        QCOMPARE(DeskWaifuLauncher::runningPid(proc() + "/missing"), -1);
    }
    void executableMatchSurvivesRenamedProcess()
    {
        process("124", "renamed");
        QVERIFY(QFile::link(app() + "/deskwaifu", proc() + "/124/exe"));
        QCOMPARE(DeskWaifuLauncher::spawn(false, app(), proc()).state, State::AlreadyRunning);
    }
    void detectsRealRunningProcess()
    {
        QProcess backend;
        backend.start(QStringLiteral(SPAWN_BACKEND_FIXTURE), QStringList{});
        QVERIFY(backend.waitForStarted());
        const auto stop = qScopeGuard([&] { backend.terminate(); backend.waitForFinished(); });
        const auto result = DeskWaifuLauncher::spawn(false, app());
        QCOMPARE(result.state, State::AlreadyRunning);
        QVERIFY(result.pid > 0);
    }
    void unrelatedAndZombieProcessesDoNotBlock()
    {
        process("123", "deskwaifu", "Z (zombie)");
        process("124", "deskwaifu-ctl");
        process("125", "deskwaifu-other");
        QCOMPARE(DeskWaifuLauncher::spawn(false, app(), proc()).state, State::Failed);
    }
    void missingExecutableAndUnreadableProcFail()
    {
        const auto result = DeskWaifuLauncher::spawn(false, app(), proc());
        QCOMPARE(result.state, State::Failed);
        QVERIFY(result.message.contains("找不到"));
        QCOMPARE(DeskWaifuLauncher::spawn(false, app(), proc() + "/missing").state, State::Failed);
    }
    void invalidExecutableReportsLaunchFailure()
    {
        QFile file(app() + "/deskwaifu");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("#!/nonexistent-deskwaifu-interpreter\n");
        file.close();
        QVERIFY(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const auto result = DeskWaifuLauncher::spawn(false, app(), proc());
        QCOMPARE(result.state, State::Failed);
        QVERIFY(result.message.contains("启动 DeskWaifu 失败"));
    }
    void lockFailureDoesNotSpawn()
    {
        installBackend(app());
        const auto result = DeskWaifuLauncher::spawn(false, app(), proc(), proc() + "/missing");
        QCOMPARE(result.state, State::Failed);
        QCOMPARE(result.pid, 0);
        QVERIFY(result.message.contains("启动锁"));
    }
    void concurrentLaunchersStartOnlyOneBackend()
    {
        // 不与用户现有后端交互。此处 PATH 和同级目录均为空，
        // 所以探测只能检查进程，不会启动程序。
        const auto before = DeskWaifuLauncher::spawn(false, app(), "/proc", proc());
        if (before.state == State::AlreadyRunning)
            QSKIP("A deskwaifu backend is already running for this user.");
        QCOMPARE(before.state, State::Failed);
        QVERIFY(before.message.contains("找不到"));
        installBackend(app());

        QLockFile gate(proc() + "/deskwaifu-spawn.lock");
        QVERIFY(gate.tryLock());
        QProcess first, second;
        const auto stopLaunchers = qScopeGuard([&] {
            for (auto* launcher : {&first, &second}) {
                if (launcher->state() != QProcess::NotRunning) {
                    launcher->kill();
                    launcher->waitForFinished();
                }
            }
        });
        const QStringList arguments{"--contend", app(), "/proc", proc()};
        first.start(QCoreApplication::applicationFilePath(), arguments);
        second.start(QCoreApplication::applicationFilePath(), arguments);
        QVERIFY(first.waitForStarted());
        QVERIFY(second.waitForStarted());
        QTRY_VERIFY(first.canReadLine());
        QCOMPARE(first.readLine().trimmed(), QByteArray("ready"));
        QTRY_VERIFY(second.canReadLine());
        QCOMPARE(second.readLine().trimmed(), QByteArray("ready"));
        // 两个独立调用方都必须在检查进程之前被锁阻塞。
        QTest::qWait(100);
        QCOMPARE(first.state(), QProcess::Running);
        QCOMPARE(second.state(), QProcess::Running);
        QVERIFY(first.readAllStandardOutput().isEmpty());
        QVERIFY(second.readAllStandardOutput().isEmpty());
        gate.unlock();

        QVERIFY(first.waitForFinished(5000));
        QVERIFY(second.waitForFinished(5000));
        const auto firstFields = first.readAllStandardOutput().trimmed().split(' ');
        const auto secondFields = second.readAllStandardOutput().trimmed().split(' ');
        QCOMPARE(firstFields.size(), 2);
        QCOMPARE(secondFields.size(), 2);
        const qint64 firstPid = firstFields[1].toLongLong();
        const qint64 secondPid = secondFields[1].toLongLong();
        const auto stopSpawned = qScopeGuard([&] {
            if (firstFields[0].toInt() == int(State::Started)) stopBackend(firstPid);
            if (secondFields[0].toInt() == int(State::Started) &&
                (firstFields[0].toInt() != int(State::Started) || secondPid != firstPid))
                stopBackend(secondPid);
        });
        QCOMPARE(first.exitStatus(), QProcess::NormalExit);
        QCOMPARE(second.exitStatus(), QProcess::NormalExit);
        QCOMPARE(first.exitCode(), 0);
        QCOMPARE(second.exitCode(), 0);
        const int firstState = firstFields[0].toInt();
        const int secondState = secondFields[0].toInt();
        QVERIFY((firstState == int(State::Started) && secondState == int(State::AlreadyRunning)) ||
                (secondState == int(State::Started) && firstState == int(State::AlreadyRunning)));
        QVERIFY(firstPid > 0);
        QCOMPARE(firstPid, secondPid);
        QVERIFY(kill(firstPid, 0) == 0);
    }
    void startsFromPath()
    {
        const QString path = directory.path() + "/path";
        installBackend(path);
        qputenv("PATH", path.toLocal8Bit());
        const auto result = DeskWaifuLauncher::spawn(false, app(), proc());
        const auto stop = qScopeGuard([&] { stopBackend(result.pid); });
        QCOMPARE(result.state, State::Started);
        QVERIFY(result.pid > 0);
        QVERIFY(kill(result.pid, 0) == 0);
    }
    void backendSurvivesLaunchingProcessExit()
    {
        installBackend(app());
        // 无效 PATH 也用于验证同级可执行文件具有更高优先级。
        qputenv("PATH", "/nonexistent-deskwaifu-path");
        QProcess gui;
        gui.start(QCoreApplication::applicationFilePath(), {"--launch-and-exit", app(), proc()});
        QVERIFY(gui.waitForFinished(5000));
        QCOMPARE(gui.exitStatus(), QProcess::NormalExit);
        QCOMPARE(gui.exitCode(), 0);
        const qint64 pid = gui.readAllStandardOutput().trimmed().toLongLong();
        const auto stop = qScopeGuard([pid] { stopBackend(pid); });
        QVERIFY(pid > 0);
        QTest::qWait(100);
        QVERIFY(kill(pid, 0) == 0);
        QFile status(QStringLiteral("/proc/%1/status").arg(pid));
        QVERIFY(status.open(QIODevice::ReadOnly));
        QVERIFY(!status.readAll().contains("State:\tZ"));

        // 第二次点击应发现已有进程，而非再次启动。
        process(QString::number(pid), "deskwaifu");
        const auto again = DeskWaifuLauncher::spawn(false, app(), proc());
        QCOMPARE(again.state, State::AlreadyRunning);
        QCOMPARE(again.pid, pid);
    }
};

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const auto arguments = application.arguments();
    if (arguments.size() == 5 && arguments[1] == "--contend")
    {
        QTextStream(stdout) << "ready" << Qt::endl;
        const auto result = DeskWaifuLauncher::spawn(false, arguments[2], arguments[3], arguments[4]);
        QTextStream(stdout) << int(result.state) << ' ' << result.pid << Qt::endl;
        return result.state == State::Started || result.state == State::AlreadyRunning ? 0 : 1;
    }
    if (arguments.size() == 4 && arguments[1] == "--launch-and-exit")
    {
        const auto result = DeskWaifuLauncher::spawn(false, arguments[2], arguments[3]);
        QTextStream(stdout) << result.pid << Qt::endl;
        return result.state == State::Started ? 0 : 1;
    }
    DeskWaifuLauncherTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "DeskWaifuLauncherTest.moc"
