#include "DeskWaifuLauncher.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
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
        QVERIFY(result.message.contains("Cannot find"));
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
        QVERIFY(result.message.contains("Failed to start"));
    }
    void startsFromPath()
    {
        const QString path = directory.path() + "/path";
        installBackend(path);
        qputenv("PATH", path.toLocal8Bit());
        const auto result = DeskWaifuLauncher::spawn(false, app(), proc());
        const auto stop = qScopeGuard([&] { if (result.pid > 0) kill(result.pid, SIGTERM); });
        QCOMPARE(result.state, State::Started);
        QVERIFY(result.pid > 0);
        QVERIFY(kill(result.pid, 0) == 0);
    }
    void backendSurvivesLaunchingProcessExit()
    {
        installBackend(app());
        // Invalid PATH also verifies that the sibling executable has priority.
        qputenv("PATH", "/nonexistent-deskwaifu-path");
        QProcess gui;
        gui.start(QCoreApplication::applicationFilePath(), {"--launch-and-exit", app(), proc()});
        QVERIFY(gui.waitForFinished(5000));
        QCOMPARE(gui.exitStatus(), QProcess::NormalExit);
        QCOMPARE(gui.exitCode(), 0);
        const qint64 pid = gui.readAllStandardOutput().trimmed().toLongLong();
        const auto stop = qScopeGuard([pid] { if (pid > 0) kill(pid, SIGTERM); });
        QVERIFY(pid > 0);
        QTest::qWait(100);
        QVERIFY(kill(pid, 0) == 0);
        QFile status(QStringLiteral("/proc/%1/status").arg(pid));
        QVERIFY(status.open(QIODevice::ReadOnly));
        QVERIFY(!status.readAll().contains("State:\tZ"));

        // A second click observes the existing process instead of launching again.
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
