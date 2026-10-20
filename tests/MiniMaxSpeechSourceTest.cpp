#include "MiniMaxSpeechSource.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <QWebSocket>
#include <QWebSocketServer>
#include <QFile>
#include <QtTest>

static void Send(QWebSocket* socket, QString event, QJsonObject data = {}, int status = 0)
{
    socket->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
        {"event", event}, {"data", data}, {"base_resp", QJsonObject{{"status_code", status}}}
    }).toJson(QJsonDocument::Compact)));
}

class MiniMaxSpeechSourceTest : public QObject
{
    Q_OBJECT
private slots:
    void liveSynthesis()
    {
        const auto path = qgetenv("DESKWAIFU_TTS_LIVE_KEY_FILE");
        if (path.isEmpty()) QSKIP("Opt-in real API test; requires DESKWAIFU_TTS_LIVE_KEY_FILE");
        QFile file(QString::fromUtf8(path));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QString key;
        for (const auto& line : file.readAll().split('\n'))
            if (line.startsWith("API_KEY=")) key = QString::fromUtf8(line.mid(8).trimmed());
        QVERIFY(!key.isEmpty());
        MiniMaxSpeechSource source(key);
        QSignalSpy errors(&source, &MiniMaxSpeechSource::Failed);
        QSignalSpy done(&source, &MiniMaxSpeechSource::Completed);
        QByteArray pcm;
        connect(&source, &MiniMaxSpeechSource::AudioReceived, this, [&](SpeechId, const QByteArray& chunk) { pcm += chunk; });
        source.Start(1, {QStringLiteral("姐姐，你好喵"), {}});
        QTRY_VERIFY_WITH_TIMEOUT(done.size() || errors.size(), 60000);
        if (!errors.isEmpty()) qWarning() << qvariant_cast<SpeechError>(errors[0][1]).message;
        QCOMPARE(errors.size(), 0);
        QCOMPARE(done.size(), 1);
        QVERIFY(pcm.size() > 4800);
        QCOMPARE(pcm.size() % 2, 0);
    }

    void bidirectionalProtocol()
    {
        QWebSocketServer server("test", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        MiniMaxSpeechSource source("test-key", nullptr, QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
        QSignalSpy ready(&source, &MiniMaxSpeechSource::Ready);
        QSignalSpy audio(&source, &MiniMaxSpeechSource::AudioReceived);
        QSignalSpy done(&source, &MiniMaxSpeechSource::Completed);
        QSignalSpy errors(&source, &MiniMaxSpeechSource::Failed);
        QStringList events;
        QString receivedText;
        QJsonObject settings;
        connect(&server, &QWebSocketServer::newConnection, this, [&] {
            auto* socket = server.nextPendingConnection();
            socket->setParent(&server);
            connect(socket, &QWebSocket::textMessageReceived, this, [&, socket](const QString& text) {
                const auto message = QJsonDocument::fromJson(text.toUtf8()).object();
                const auto event = message.value("event").toString();
                events << event;
                if (event == "task_start") { settings = message; Send(socket, "task_started"); }
                if (event == "task_continue") receivedText += message.value("text").toString();
                if (event == "task_finish") {
                    Send(socket, "sentence_start");
                    Send(socket, "task_continued", {{"audio", "000102"}});
                    Send(socket, "task_continued", {{"audio", "03"}});
                    Send(socket, "sentence_end");
                    Send(socket, "task_finished");
                }
            });
            Send(socket, "connected_success");
        });
        source.Start(1, {QStringLiteral("姐姐，你好喵"), {}});
        QTRY_COMPARE(done.size(), 1);
        QCOMPARE(errors.size(), 0);
        QCOMPARE(ready.size(), 1);
        const auto format = qvariant_cast<QAudioFormat>(ready[0][1]);
        QCOMPARE(format.sampleRate(), 24000);
        QCOMPARE(format.sampleFormat(), QAudioFormat::Int16);
        QCOMPARE(audio.size(), 2);
        QCOMPARE(audio[0][1].toByteArray() + audio[1][1].toByteArray(), QByteArray::fromHex("00010203"));
        QCOMPARE(receivedText, QStringLiteral("姐姐，你好喵"));
        QCOMPARE(events, QStringList({"task_start", "task_continue", "task_finish"}));
        QCOMPARE(settings.value("audio_setting").toObject().value("format").toString(), QString("pcm"));
    }

    void rejectsInvalidResponse_data()
    {
        QTest::addColumn<QString>("response");
        QTest::newRow("invalid-json") << "not-json";
        QTest::newRow("authentication") << R"({"event":"error","base_resp":{"status_code":1004,"status_msg":"test-key"}})";
        QTest::newRow("wrong-order") << R"({"event":"task_finished","base_resp":{"status_code":0}})";
    }

    void rejectsInvalidResponse()
    {
        QFETCH(QString, response);
        QWebSocketServer server("test", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        MiniMaxSpeechSource source("test-key", nullptr, QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
        QSignalSpy errors(&source, &MiniMaxSpeechSource::Failed);
        QSignalSpy done(&source, &MiniMaxSpeechSource::Completed);
        connect(&server, &QWebSocketServer::newConnection, this, [&] {
            auto* socket = server.nextPendingConnection();
            socket->setParent(&server);
            socket->sendTextMessage(response);
        });
        source.Start(2, {"test", {}});
        QTRY_COMPARE(errors.size(), 1);
        QVERIFY(!qvariant_cast<SpeechError>(errors[0][1]).message.contains("test-key"));
        QCOMPARE(done.size(), 0);
    }

    void missingKeyAndInvalidOptions()
    {
        MiniMaxSpeechSource source({});
        QSignalSpy errors(&source, &MiniMaxSpeechSource::Failed);
        source.Start(1, {"test", {}});
        QCOMPARE(errors.size(), 1);
        QCOMPARE(qvariant_cast<SpeechError>(errors[0][1]).code, SpeechErrorCode::MissingApiKey);
        SpeechRequest request{"test", {}};
        request.options.speed = 100;
        source.Start(2, request);
        QCOMPARE(errors.size(), 2);
        QCOMPARE(qvariant_cast<SpeechError>(errors[1][1]).code, SpeechErrorCode::InvalidRequest);
    }

    void cancelDoesNotComplete()
    {
        QWebSocketServer server("test", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        MiniMaxSpeechSource source("key", nullptr, QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
        QSignalSpy errors(&source, &MiniMaxSpeechSource::Failed);
        QSignalSpy done(&source, &MiniMaxSpeechSource::Completed);
        source.Start(7, {"test", {}});
        source.Cancel(6); // Must not cancel another task.
        QTRY_VERIFY(server.hasPendingConnections());
        auto* socket = server.nextPendingConnection();
        socket->setParent(&server);
        source.Cancel(7);
        Send(socket, "connected_success");
        QTest::qWait(20);
        QCOMPARE(errors.size(), 0);
        QCOMPARE(done.size(), 0);
    }

    void invalidAudio_data()
    {
        QTest::addColumn<QJsonValue>("audio");
        QTest::newRow("odd-length") << QJsonValue("a");
        QTest::newRow("invalid-hex") << QJsonValue("xx");
        QTest::newRow("unicode") << QJsonValue(QStringLiteral("你好"));
        QTest::newRow("wrong-type") << QJsonValue(42);
    }

    void invalidAudio()
    {
        QFETCH(QJsonValue, audio);
        QWebSocketServer server("test", QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        MiniMaxSpeechSource source("key", nullptr, QUrl(QString("ws://127.0.0.1:%1").arg(server.serverPort())));
        QSignalSpy errors(&source, &MiniMaxSpeechSource::Failed);
        QSignalSpy chunks(&source, &MiniMaxSpeechSource::AudioReceived);
        connect(&server, &QWebSocketServer::newConnection, this, [&] {
            auto* socket = server.nextPendingConnection();
            socket->setParent(&server);
            connect(socket, &QWebSocket::textMessageReceived, this, [&, socket](const QString& text) {
                const auto event = QJsonDocument::fromJson(text.toUtf8()).object().value("event").toString();
                if (event == "task_start") Send(socket, "task_started");
                if (event == "task_finish") Send(socket, "task_continued", {{"audio", audio}});
            });
            Send(socket, "connected_success");
        });
        source.Start(1, {"test", {}});
        QTRY_COMPARE(errors.size(), 1);
        QCOMPARE(qvariant_cast<SpeechError>(errors[0][1]).code, SpeechErrorCode::Protocol);
        QCOMPARE(chunks.size(), 0);
    }
};

QTEST_GUILESS_MAIN(MiniMaxSpeechSourceTest)
#include "MiniMaxSpeechSourceTest.moc"
