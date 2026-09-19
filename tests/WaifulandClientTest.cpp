#include "WaifulandClient.hpp"

#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class WaifulandClientTest : public QObject
{
    Q_OBJECT

private slots:
    void sendsJsonAndParsesResponse();
    void doesNotReplayUnacknowledgedMutation();
    void retriesUnacknowledgedQuery();
};

void WaifulandClientTest::sendsJsonAndParsesResponse()
{
    QTemporaryDir runtimeDir;
    QVERIFY(runtimeDir.isValid());
    qputenv("XDG_RUNTIME_DIR", runtimeDir.path().toLocal8Bit());

    const QString socketPath = QDir(runtimeDir.path()).filePath(QStringLiteral("waifuland.sock"));
    QLocalServer server;
    QVERIFY2(server.listen(socketPath), qPrintable(server.errorString()));

    WaifulandClient client;
    QSignalSpy responseSpy(&client, &WaifulandClient::responseReceived);
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

void WaifulandClientTest::doesNotReplayUnacknowledgedMutation()
{
    QTemporaryDir runtimeDir;
    QVERIFY(runtimeDir.isValid());
    qputenv("XDG_RUNTIME_DIR", runtimeDir.path().toLocal8Bit());

    const QString socketPath = QDir(runtimeDir.path()).filePath(QStringLiteral("waifuland.sock"));
    QLocalServer server;
    QVERIFY2(server.listen(socketPath), qPrintable(server.errorString()));

    WaifulandClient client;
    QSignalSpy unknownSpy(&client, &WaifulandClient::commandOutcomeUnknown);
    client.sendCommand(QStringLiteral("toggle_hidden"));
    client.sendCommand(QStringLiteral("get_status"));

    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 2000);
    QLocalSocket* firstConnection = server.nextPendingConnection();
    QVERIFY(firstConnection);
    QTRY_VERIFY_WITH_TIMEOUT(firstConnection->canReadLine(), 2000);
    const QJsonObject firstRequest = QJsonDocument::fromJson(firstConnection->readLine()).object();
    QCOMPARE(firstRequest.value(QStringLiteral("command")).toString(), QStringLiteral("toggle_hidden"));

    firstConnection->disconnectFromServer();
    QTRY_COMPARE_WITH_TIMEOUT(unknownSpy.count(), 1, 2000);
    QCOMPARE(unknownSpy.takeFirst().at(0).toString(), QStringLiteral("toggle_hidden"));

    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 4000);
    QLocalSocket* secondConnection = server.nextPendingConnection();
    QVERIFY(secondConnection);
    QTRY_VERIFY_WITH_TIMEOUT(secondConnection->canReadLine(), 2000);
    const QJsonObject secondRequest = QJsonDocument::fromJson(secondConnection->readLine()).object();
    QCOMPARE(secondRequest.value(QStringLiteral("command")).toString(), QStringLiteral("get_status"));

    secondConnection->write("{\"ok\":true}\n");
    secondConnection->flush();
}

void WaifulandClientTest::retriesUnacknowledgedQuery()
{
    QTemporaryDir runtimeDir;
    QVERIFY(runtimeDir.isValid());
    qputenv("XDG_RUNTIME_DIR", runtimeDir.path().toLocal8Bit());

    const QString socketPath = QDir(runtimeDir.path()).filePath(QStringLiteral("waifuland.sock"));
    QLocalServer server;
    QVERIFY2(server.listen(socketPath), qPrintable(server.errorString()));

    WaifulandClient client;
    QSignalSpy unknownSpy(&client, &WaifulandClient::commandOutcomeUnknown);
    client.sendCommand(QStringLiteral("get_status"));

    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 2000);
    QLocalSocket* firstConnection = server.nextPendingConnection();
    QVERIFY(firstConnection);
    QTRY_VERIFY_WITH_TIMEOUT(firstConnection->canReadLine(), 2000);
    QCOMPARE(QJsonDocument::fromJson(firstConnection->readLine()).object()
        .value(QStringLiteral("command")).toString(), QStringLiteral("get_status"));
    firstConnection->disconnectFromServer();

    QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 4000);
    QLocalSocket* secondConnection = server.nextPendingConnection();
    QVERIFY(secondConnection);
    QTRY_VERIFY_WITH_TIMEOUT(secondConnection->canReadLine(), 2000);
    QCOMPARE(QJsonDocument::fromJson(secondConnection->readLine()).object()
        .value(QStringLiteral("command")).toString(), QStringLiteral("get_status"));
    QCOMPARE(unknownSpy.count(), 0);

    secondConnection->write("{\"ok\":true}\n");
    secondConnection->flush();
}

QTEST_GUILESS_MAIN(WaifulandClientTest)
#include "WaifulandClientTest.moc"
