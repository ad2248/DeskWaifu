#include "LoadedGuiTransport.hpp"
#include "plugin/GuiPluginManager.hpp"
#include <QTabWidget>

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
#include <csignal>
#include <stdexcept>
#include <unistd.h>

class RespawnerFixture final : public QObject
{
    Q_OBJECT
public:
    RespawnerFixture(GuiHostServices& services, const QString& directory)
    {
        const auto root = _installation.path();
        if (!_installation.isValid() || !QDir().mkpath(root + "/system_fixture") ||
            !QFile::copy(QStringLiteral(SYSTEM_RESPAWNER_FIXTURE), root + "/system_fixture/system_fixture.so"))
            throw std::runtime_error("install system respawner fixture");
        _plugins.reset(new GuiPluginManager(root, services, _tabs));
        QString error;
        if (!_plugins->Start(&error)) throw std::runtime_error(error.toStdString());
        _page = _tabs.findChild<QWidget*>("respawnerFixture");
        if (!_page || !QMetaObject::invokeMethod(_page, "configure", Qt::DirectConnection, Q_ARG(QString, directory)))
            throw std::runtime_error("configure public respawner fixture");
        connect(_page, SIGNAL(finished(bool,QString)), this, SIGNAL(finished(bool,QString)));
    }
    bool isRunning() const { return _page->property("running").toBool(); }
    void start(const QString& proc = QStringLiteral("/proc"), const QString& locks = {}, int timeout = 10000)
    {
        if (!QMetaObject::invokeMethod(_page, "start", Qt::DirectConnection, Q_ARG(QString, proc), Q_ARG(QString, locks), Q_ARG(int, timeout)))
            throw std::runtime_error("start public respawner fixture");
    }
    void stop() { QMetaObject::invokeMethod(_page, "stop", Qt::DirectConnection); }
signals:
    void finished(bool success, const QString& message);
private:
    QTemporaryDir _installation;
    QTabWidget _tabs;
    std::unique_ptr<GuiPluginManager> _plugins;
    QWidget* _page = nullptr;
};

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
    static QLocalSocket* connectClient(QLocalServer& server, LoadedGuiTransport& client)
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
        LoadedGuiTransport client;
        RespawnerFixture respawner(client, app());
        QSignalSpy finished(&respawner, &RespawnerFixture::finished);
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
        QVERIFY(finished[0][1].toString().contains("未停止"));
        QTest::qWait(100);
        QVERIFY(!socket->canReadLine());
    }
    void waitsForProcessExitAndReconnects_data()
    {
        QTest::addColumn<bool>("acknowledged");
        QTest::newRow("acknowledged") << true;
        QTest::newRow("lost-reply-with-queued-mutations") << false;
    }
    void waitsForProcessExitAndReconnects()
    {
        QFETCH(bool, acknowledged);
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
        LoadedGuiTransport client;
        QVERIFY(server.listen(client.socketPath()));
        auto* socket = connectClient(server, client);
        QVERIFY(socket);
        RespawnerFixture respawner(client, app());
        QSignalSpy finished(&respawner, &RespawnerFixture::finished);
        respawner.start(proc(), directory.path());
        respawner.start(proc(), directory.path()); // 重复点击不能再次排入 quit 命令。
        QTRY_VERIFY(socket->canReadLine());
        QCOMPARE(QJsonDocument::fromJson(socket->readLine()).object().value("command").toString(), "quit");
        QSignalSpy cancelled(&client, &GuiHostServices::commandCancelled);
        if (acknowledged) socket->write("{\"ok\":true}\n");
        else {
            // 旧后端退出，既不确认 quit，也不处理其排队命令。
            for (const auto& command : {"toggle_hidden", "next_model", "do_motion", "set_output"})
                client.sendCommand(QString::fromLatin1(command));
        }
        socket->disconnectFromServer();
        QTRY_COMPARE(cancelled.size(), acknowledged ? 0 : 4);
        QTest::qWait(350);
        QVERIFY(respawner.isRunning());
        QCOMPARE(finished.size(), 0); // IPC 已关闭，但进程仍存活。
        server.close(); // 替代进程的 IPC 端点不会立即就绪。
        oldBackend.terminate();
        QVERIFY(oldBackend.waitForFinished());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 3000);
        const QString message = finished[0][1].toString();
        const qint64 newPid = message.section("PID ", 1).section(QChar(0xff09), 0, 0).toLongLong();
        const auto stopNew = qScopeGuard([&] {
            if (newPid > 0) kill(newPid, SIGKILL);
        });
        QVERIFY2(finished[0][0].toBool(), qPrintable(message));
        QVERIFY(newPid > 0);
        QVERIFY(kill(newPid, 0) == 0);
        QTest::qWait(3400); // 连接失败不能终止自动重试。
        QVERIFY(!client.isConnected());
        QVERIFY(server.listen(client.socketPath()));
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() && client.isConnected(), 3000);
        auto* replacement = server.nextPendingConnection();
        QTest::qWait(150);
        QVERIFY(!replacement->canReadLine()); // quit 不能发送给替代进程。
        QVERIFY(!respawner.isRunning());
    }
    void quitRejected()
    {
        installBackend();
        QLocalServer server;
        LoadedGuiTransport client;
        QVERIFY(server.listen(client.socketPath()));
        auto* socket = connectClient(server, client);
        QVERIFY(socket);
        RespawnerFixture respawner(client, app());
        QSignalSpy finished(&respawner, &RespawnerFixture::finished);
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
        LoadedGuiTransport client;
        QVERIFY(server.listen(client.socketPath()));
        auto* socket = connectClient(server, client);
        QVERIFY(socket);
        RespawnerFixture respawner(client, app());
        QSignalSpy finished(&respawner, &RespawnerFixture::finished);
        respawner.start(proc(), directory.path(), 400);
        QTRY_VERIFY(socket->canReadLine());
        socket->readLine();
        socket->disconnectFromServer();
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished[0][0].toBool());
        QVERIFY(finished[0][1].toString().contains("超时"));
        QVERIFY(!respawner.isRunning());
    }
    void timeoutCancelsQueuedQuit()
    {
        installBackend();
        QLocalServer server;
        LoadedGuiTransport client;
        QVERIFY(server.listen(client.socketPath()));
        auto* socket = connectClient(server, client);
        QVERIFY(socket);
        client.sendCommand("get_status"); // 将 quit 排在永不回复的查询之后。
        RespawnerFixture respawner(client, app());
        QSignalSpy finished(&respawner, &RespawnerFixture::finished);
        respawner.start(proc(), directory.path(), 200);
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished[0][0].toBool());
        QVERIFY(finished[0][1].toString().contains("超时"));
        QTRY_VERIFY(socket->canReadLine());
        socket->readLine();
        socket->write("{\"ok\":true}\n");
        socket->flush();
        QTest::qWait(200);
        QVERIFY(!socket->canReadLine());
        QVERIFY(client.isConnected());
    }

    void stopCancelsQueuedQuitWithoutFeedback()
    {
        installBackend();
        QLocalServer server;
        LoadedGuiTransport client;
        QVERIFY(server.listen(client.socketPath()));
        auto* socket = connectClient(server, client);
        QVERIFY(socket);
        client.sendCommand("get_status");
        RespawnerFixture respawner(client, app());
        QSignalSpy finished(&respawner, &RespawnerFixture::finished);
        respawner.start(proc(), directory.path());
        QVERIFY(respawner.isRunning());
        respawner.stop();
        respawner.stop();
        QVERIFY(!respawner.isRunning());
        QVERIFY(finished.isEmpty());
        QTRY_VERIFY(socket->canReadLine());
        QCOMPARE(QJsonDocument::fromJson(socket->readLine()).object().value("command").toString(), "get_status");
        socket->write("{\"ok\":true}\n");
        socket->flush();
        QTest::qWait(200);
        QVERIFY(!socket->canReadLine());
    }
};

QTEST_MAIN(DeskWaifuRespawnerTest)
#include "DeskWaifuRespawnerTest.moc"
