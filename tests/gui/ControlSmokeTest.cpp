#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>
#include <fcntl.h>
#include <unistd.h>

class ControlSmokeTest : public QObject
{
    Q_OBJECT
private slots:
    void probesWithoutSystemPlugin_data()
    {
        QTest::addColumn<bool>("reply");
        QTest::addColumn<QJsonObject>("response");
        QTest::addColumn<int>("exitCode");
        QTest::newRow("successful-roundtrip") << true << QJsonObject{{"ok", true}} << 0;
        QTest::newRow("backend-rejected") << true << QJsonObject{{"ok", false}} << 1;
        QTest::newRow("invalid-success-type") << true << QJsonObject{{"ok", "true"}} << 1;
        QTest::newRow("connected-but-silent") << false << QJsonObject{} << -1;
    }

    void probesWithoutSystemPlugin()
    {
        QFETCH(bool, reply);
        QFETCH(QJsonObject, response);
        QFETCH(int, exitCode);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const int fd = open(QFile::encodeName(directory.path()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        QVERIFY(fd >= 0);
        const auto closeDirectory = qScopeGuard([fd] { close(fd); });
        // The child uses the parent's descriptor path, keeping Unix socket
        // names short even when TMPDIR is longer than sockaddr_un allows.
        const auto runtime = QStringLiteral("/proc/%1/fd/%2").arg(QCoreApplication::applicationPid()).arg(fd);
        QLocalServer server;
        QVERIFY(server.listen(runtime + QStringLiteral("/deskwaifu.sock")));
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime);
        environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
        environment.insert(QStringLiteral("DESKWAIFU_CONTROL_PLUGIN_DIR"), QString());
        child.setProcessEnvironment(environment);
        child.start(QStringLiteral(CONTROL_BINARY), {QStringLiteral("--smoke-test")});
        const auto stopChild = qScopeGuard([&] {
            if (child.state() != QProcess::NotRunning) {
                child.kill();
                child.waitForFinished();
            }
        });
        QVERIFY2(child.waitForStarted(), qPrintable(child.errorString()));
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 3000);
        auto* socket = server.nextPendingConnection();
        QTRY_VERIFY_WITH_TIMEOUT(socket->canReadLine(), 3000);
        const auto request = QJsonDocument::fromJson(socket->readLine()).object();
        QCOMPARE(request.value(QStringLiteral("command")).toString(), QStringLiteral("get_status"));
        if (!reply) {
            QTest::qWait(250);
            QVERIFY(child.state() != QProcess::NotRunning); // Socket connect alone is not success.
            return;
        }
        socket->write(QJsonDocument(response).toJson(QJsonDocument::Compact) + '\n');
        socket->flush();
        QTRY_VERIFY_WITH_TIMEOUT(child.state() == QProcess::NotRunning, 5000);
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), exitCode);
    }
};

QTEST_GUILESS_MAIN(ControlSmokeTest)
#include "ControlSmokeTest.moc"
