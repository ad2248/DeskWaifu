#include "DeskWaifuClient.hpp"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class DeskWaifuClientTest : public QObject
{
    Q_OBJECT

private slots:
    void sendsJsonAndParsesResponse();
    void doesNotReplayUnacknowledgedMutation();
    void doesNotReplayUnacknowledgedMutation_data();
    void retriesUnacknowledgedQuery();
    void retriesUnacknowledgedQuery_data();
    void reconnectsAfterFailedAttempts();
    void reconnectsAfterFailedAttempts_data();
};

void DeskWaifuClientTest::reconnectsAfterFailedAttempts_data()
{
    QTest::addColumn<bool>("restart");
    QTest::addColumn<bool>("staleSocket");
    QTest::newRow("initial-missing") << false << false;
    QTest::newRow("initial-stale") << false << true;
    QTest::newRow("restart-missing") << true << false;
    QTest::newRow("restart-stale") << true << true;
}

void DeskWaifuClientTest::reconnectsAfterFailedAttempts()
{
    QFETCH(bool, restart);
    QFETCH(bool, staleSocket);
    QTemporaryDir runtimeDir;
    QVERIFY(runtimeDir.isValid());
    const auto oldRuntime = qgetenv("XDG_RUNTIME_DIR");
    const auto restoreRuntime = qScopeGuard([&] { qputenv("XDG_RUNTIME_DIR", oldRuntime); });
    qputenv("XDG_RUNTIME_DIR", runtimeDir.path().toLocal8Bit());
    DeskWaifuClient client;
    QLocalServer server;
    if (restart || staleSocket) {
        QVERIFY(server.listen(client.socketPath()));
        if (restart) {
            client.connectToServer();
            QTRY_VERIFY(server.hasPendingConnections() && client.isConnected());
            server.nextPendingConnection()->abort();
            QTRY_VERIFY(!client.isConnected());
        }
        // Preserve a filesystem entry without a listener to simulate a stale socket.
        if (staleSocket) QVERIFY(QFile::rename(client.socketPath(), client.socketPath() + ".stale"));
        server.close();
        if (staleSocket) QVERIFY(QFile::rename(client.socketPath() + ".stale", client.socketPath()));
    }
    QSignalSpy connectionSpy(&client, &DeskWaifuClient::connectionChanged);
    if (!restart) client.connectToServer();
    QTest::qWait(3400); // The backend is unavailable for more than two retry intervals.
    QVERIFY(!client.isConnected());
    QVERIFY(connectionSpy.size() >= 2);
    QVERIFY(QLocalServer::removeServer(client.socketPath()));
    QVERIFY(server.listen(client.socketPath()));
    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections() && client.isConnected(), 4000);
    auto* replacement = server.nextPendingConnection();
    client.sendCommand(QStringLiteral("get_status"));
    QTRY_VERIFY(replacement->canReadLine());
    QCOMPARE(QJsonDocument::fromJson(replacement->readLine()).object().value("command").toString(), "get_status");
}

void DeskWaifuClientTest::sendsJsonAndParsesResponse()
{
    QTemporaryDir runtimeDir;
    QVERIFY(runtimeDir.isValid());
    qputenv("XDG_RUNTIME_DIR", runtimeDir.path().toLocal8Bit());

    const QString socketPath = QDir(runtimeDir.path()).filePath(QStringLiteral("deskwaifu.sock"));
    QLocalServer server;
    QVERIFY2(server.listen(socketPath), qPrintable(server.errorString()));

    DeskWaifuClient client;
    QSignalSpy responseSpy(&client, &DeskWaifuClient::responseReceived);
    client.sendCommand(QStringLiteral("set_model_position"), {
        {QStringLiteral("x"), 0.25},
        {QStringLiteral("y"), -0.5}
    });

    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 2000);
    QLocalSocket* connection = server.nextPendingConnection();
    QVERIFY(connection);
    QTRY_VERIFY_WITH_TIMEOUT(connection->canReadLine(), 2000);

    QJsonParseError parseError;
    const QJsonObject request = QJsonDocument::fromJson(connection->readLine(), &parseError).object();
    QCOMPARE(parseError.error, QJsonParseError::NoError);
    QCOMPARE(request.value(QStringLiteral("command")).toString(), QStringLiteral("set_model_position"));
    QCOMPARE(request.value(QStringLiteral("x")).toDouble(), 0.25);
    QCOMPARE(request.value(QStringLiteral("y")).toDouble(), -0.5);

    connection->write("{\"ok\":true,\"x\":0.25,\"y\":-0.5}\n");
    connection->flush();
    QTRY_COMPARE_WITH_TIMEOUT(responseSpy.count(), 1, 2000);

    const QList<QVariant> arguments = responseSpy.takeFirst();
    QCOMPARE(arguments.at(0).toString(), QStringLiteral("set_model_position"));
    const QJsonObject response = arguments.at(1).toJsonObject();
    QVERIFY(response.value(QStringLiteral("ok")).toBool());
    QCOMPARE(response.value(QStringLiteral("x")).toDouble(), 0.25);
}

void DeskWaifuClientTest::doesNotReplayUnacknowledgedMutation()
{
    QFETCH(QString, command);
    QTemporaryDir runtimeDir;
    QVERIFY(runtimeDir.isValid());
    qputenv("XDG_RUNTIME_DIR", runtimeDir.path().toLocal8Bit());

    const QString socketPath = QDir(runtimeDir.path()).filePath(QStringLiteral("deskwaifu.sock"));
    QLocalServer server;
    QVERIFY2(server.listen(socketPath), qPrintable(server.errorString()));

    DeskWaifuClient client;
    QSignalSpy unknownSpy(&client, &DeskWaifuClient::commandOutcomeUnknown);
    client.sendCommand(command);
    client.sendCommand(QStringLiteral("trigger_hotkey"));
    client.sendCommand(QStringLiteral("set_hover_opacity"), {{QStringLiteral("opacity"), 0.5}});
    client.sendCommand(QStringLiteral("set_mouse_look"), {{QStringLiteral("radius"), 0.5}});
    client.sendCommand(QStringLiteral("quit"));
    client.sendCommand(QStringLiteral("get_status"));

    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 2000);
    QLocalSocket* firstConnection = server.nextPendingConnection();
    QVERIFY(firstConnection);
    QTRY_VERIFY_WITH_TIMEOUT(firstConnection->canReadLine(), 2000);
    const QJsonObject firstRequest = QJsonDocument::fromJson(firstConnection->readLine()).object();
    QCOMPARE(firstRequest.value(QStringLiteral("command")).toString(), command);

    firstConnection->disconnectFromServer();
    QTRY_COMPARE_WITH_TIMEOUT(unknownSpy.count(), 1, 2000);
    QCOMPARE(unknownSpy.takeFirst().at(0).toString(), command);

    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 4000);
    QLocalSocket* secondConnection = server.nextPendingConnection();
    QVERIFY(secondConnection);
    QTRY_VERIFY_WITH_TIMEOUT(secondConnection->canReadLine(), 2000);
    const QJsonObject secondRequest = QJsonDocument::fromJson(secondConnection->readLine()).object();
    QCOMPARE(secondRequest.value(QStringLiteral("command")).toString(), QStringLiteral("get_status"));

    secondConnection->write("{\"ok\":true}\n");
    secondConnection->flush();
}

void DeskWaifuClientTest::doesNotReplayUnacknowledgedMutation_data()
{
    QTest::addColumn<QString>("command");
    QTest::newRow("visibility") << QStringLiteral("toggle_hidden");
    QTest::newRow("model-hotkey") << QStringLiteral("trigger_hotkey");
    QTest::newRow("transform-apply") << QStringLiteral("apply_transform_edit");
    QTest::newRow("transform-reset") << QStringLiteral("reset_transform_settings");
    QTest::newRow("hover-opacity") << QStringLiteral("set_hover_opacity");
    QTest::newRow("mouse-look") << QStringLiteral("set_mouse_look");
    QTest::newRow("quit") << QStringLiteral("quit");
}

void DeskWaifuClientTest::retriesUnacknowledgedQuery()
{
    QFETCH(QString, command);
    QTemporaryDir runtimeDir;
    QVERIFY(runtimeDir.isValid());
    qputenv("XDG_RUNTIME_DIR", runtimeDir.path().toLocal8Bit());

    const QString socketPath = QDir(runtimeDir.path()).filePath(QStringLiteral("deskwaifu.sock"));
    QLocalServer server;
    QVERIFY2(server.listen(socketPath), qPrintable(server.errorString()));

    DeskWaifuClient client;
    QSignalSpy unknownSpy(&client, &DeskWaifuClient::commandOutcomeUnknown);
    client.sendCommand(command);

    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 2000);
    QLocalSocket* firstConnection = server.nextPendingConnection();
    QVERIFY(firstConnection);
    QTRY_VERIFY_WITH_TIMEOUT(firstConnection->canReadLine(), 2000);
    QCOMPARE(QJsonDocument::fromJson(firstConnection->readLine()).object()
        .value(QStringLiteral("command")).toString(), command);
    firstConnection->disconnectFromServer();

    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 4000);
    QLocalSocket* secondConnection = server.nextPendingConnection();
    QVERIFY(secondConnection);
    QTRY_VERIFY_WITH_TIMEOUT(secondConnection->canReadLine(), 2000);
    QCOMPARE(QJsonDocument::fromJson(secondConnection->readLine()).object()
        .value(QStringLiteral("command")).toString(), command);
    QCOMPARE(unknownSpy.count(), 0);

    secondConnection->write("{\"ok\":true}\n");
    secondConnection->flush();
}

void DeskWaifuClientTest::retriesUnacknowledgedQuery_data()
{
    QTest::addColumn<QString>("command");
    QTest::newRow("status") << QStringLiteral("get_status");
    QTest::newRow("hover-opacity") << QStringLiteral("get_hover_opacity");
    QTest::newRow("mouse-look") << QStringLiteral("get_mouse_look");
}

QTEST_GUILESS_MAIN(DeskWaifuClientTest)
#include "DeskWaifuClientTest.moc"
