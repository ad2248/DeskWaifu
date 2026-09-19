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

QTEST_GUILESS_MAIN(WaifulandClientTest)
#include "WaifulandClientTest.moc"
