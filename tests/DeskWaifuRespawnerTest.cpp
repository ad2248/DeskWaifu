#include "DeskWaifuRespawner.hpp"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLocalServer>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
#include <csignal>
#include <unistd.h>

class DeskWaifuRespawnerTest : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir directory{QDir::tempPath() + "/dw-respawn-XXXXXX"};
    QByteArray oldRuntime, oldPath;
    QString app() const { return directory.path() + "/app"; }
    QString proc() const { return directory.path() + "/proc"; }
    void installBackend()
    {
        QVERIFY(QFile::copy(QStringLiteral(SPAWN_BACKEND_FIXTURE), app() + "/deskwaifu"));
    }
    static QLocalSocket* connectClient(QLocalServer& server, DeskWaifuClient& client)
    {
        client.connectToServer();
        if (!QTest::qWaitFor([&] { return server.hasPendingConnections() && client.isConnected(); }, 2000))
            return nullptr;
        return server.nextPendingConnection();
    }

private slots:
    void init()
    {
        QVERIFY(directory.isValid());
        QVERIFY(QDir().mkpath(app()));
        QVERIFY(QDir().mkpath(proc()));
        oldRuntime = qgetenv("XDG_RUNTIME_DIR");
        oldPath = qgetenv("PATH");
        qputenv("XDG_RUNTIME_DIR", directory.path().toLocal8Bit());
        qputenv("PATH", app().toLocal8Bit());
    }
    void cleanup()
    {
        qputenv("XDG_RUNTIME_DIR", oldRuntime);
        qputenv("PATH", oldPath);
        QVERIFY(QDir(app()).removeRecursively());
        QVERIFY(QDir(proc()).removeRecursively());
        QFile::remove(directory.path() + "/deskwaifu.sock");
    }
    void requiresConnectionAndExecutable()
    {
        DeskWaifuClient client;
        DeskWaifuRespawner respawner(client, app());
        QSignalSpy finished(&respawner, &DeskWaifuRespawner::finished);
        respawner.start();
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.takeFirst()[0].toBool());
        QLocalServer server;
        QVERIFY(server.listen(client.socketPath()));
        auto* socket = connectClient(server, client);
        QVERIFY(socket);
        respawner.start();
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished[0][0].toBool());
        QVERIFY(finished[0][1].toString().contains("not stopped"));
        QTest::qWait(100);
        QVERIFY(!socket->canReadLine());
    }
    void waitsForProcessExitAndReconnects()
    {
        installBackend();
        QProcess oldBackend;
        oldBackend.start(QStringLiteral(SPAWN_BACKEND_FIXTURE), QStringList{});
        QVERIFY(oldBackend.waitForStarted());
        const auto stopOld = qScopeGuard([&] {
            if (oldBackend.state() != QProcess::NotRunning) {
                oldBackend.kill(); oldBackend.waitForFinished();
            }
        });
        QVERIFY(QFile::link(QStringLiteral("/proc/%1").arg(oldBackend.processId()), proc() + "/123"));
        QLocalServer server;
        DeskWaifuClient client;
        QVERIFY(server.listen(client.socketPath()));
        auto* socket = connectClient(server, client);
        QVERIFY(socket);
        DeskWaifuRespawner respawner(client, app());
        QSignalSpy finished(&respawner, &DeskWaifuRespawner::finished);
        respawner.start(proc(), directory.path());
        respawner.start(proc(), directory.path()); // A repeated click must not queue another quit.
        QTRY_VERIFY(socket->canReadLine());
        QCOMPARE(QJsonDocument::fromJson(socket->readLine()).object().value("command").toString(), "quit");
        socket->write("{\"ok\":true}\n");
        socket->disconnectFromServer();
        QTest::qWait(350);
        QVERIFY(respawner.isRunning());
        QCOMPARE(finished.size(), 0); // IPC closed, but process still alive.
        oldBackend.terminate();
        QVERIFY(oldBackend.waitForFinished());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
        const QString message = finished[0][1].toString();
        const qint64 newPid = message.section("PID ", 1).section(')', 0, 0).toLongLong();
        const auto stopNew = qScopeGuard([&] {
            if (newPid > 0) kill(newPid, SIGKILL);
        });
        QVERIFY2(finished[0][0].toBool(), qPrintable(message));
        QVERIFY(newPid > 0);
        QVERIFY(kill(newPid, 0) == 0);
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() && client.isConnected(), 3000);
        auto* replacement = server.nextPendingConnection();
        QTest::qWait(150);
        QVERIFY(!replacement->canReadLine()); // Quit must never reach the replacement.
        QVERIFY(!respawner.isRunning());
    }
    void quitRejected()
    {
        installBackend();
        QLocalServer server;
        DeskWaifuClient client;
        QVERIFY(server.listen(client.socketPath()));
        auto* socket = connectClient(server, client);
        QVERIFY(socket);
        DeskWaifuRespawner respawner(client, app());
        QSignalSpy finished(&respawner, &DeskWaifuRespawner::finished);
        respawner.start(proc(), directory.path());
        QTRY_VERIFY(socket->canReadLine());
        socket->readLine();
        socket->write("{\"ok\":false,\"error\":\"not supported\"}\n");
        socket->flush();
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished[0][0].toBool());
        QVERIFY(finished[0][1].toString().contains("not supported"));
        QVERIFY(client.isConnected());
        QVERIFY(!respawner.isRunning());
    }
    void disconnectedProcessMustStillExit()
    {
        installBackend();
        QVERIFY(QDir().mkpath(proc() + "/123"));
        QFile status(proc() + "/123/status");
        QVERIFY(status.open(QIODevice::WriteOnly));
        status.write("Name:\tdeskwaifu\nState:\tS (sleeping)\n");
        status.close();
        QLocalServer server;
        DeskWaifuClient client;
        QVERIFY(server.listen(client.socketPath()));
        auto* socket = connectClient(server, client);
        QVERIFY(socket);
        DeskWaifuRespawner respawner(client, app());
        QSignalSpy finished(&respawner, &DeskWaifuRespawner::finished);
        respawner.start(proc(), directory.path(), 400);
        QTRY_VERIFY(socket->canReadLine());
        socket->readLine();
        socket->disconnectFromServer();
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished[0][0].toBool());
        QVERIFY(finished[0][1].toString().contains("timed out"));
        QVERIFY(!respawner.isRunning());
    }
    void timeoutCancelsQueuedQuit()
    {
        installBackend();
        QLocalServer server;
        DeskWaifuClient client;
        QVERIFY(server.listen(client.socketPath()));
        auto* socket = connectClient(server, client);
        QVERIFY(socket);
        client.sendCommand("get_status"); // Hold quit behind a query that never replies.
        DeskWaifuRespawner respawner(client, app());
        QSignalSpy finished(&respawner, &DeskWaifuRespawner::finished);
        respawner.start(proc(), directory.path(), 200);
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished[0][0].toBool());
        QVERIFY(finished[0][1].toString().contains("timed out"));
        QTRY_VERIFY(socket->canReadLine());
        socket->readLine();
        socket->write("{\"ok\":true}\n");
        socket->flush();
        QTest::qWait(200);
        QVERIFY(!socket->canReadLine());
        QVERIFY(client.isConnected());
    }
};

QTEST_GUILESS_MAIN(DeskWaifuRespawnerTest)
#include "DeskWaifuRespawnerTest.moc"
