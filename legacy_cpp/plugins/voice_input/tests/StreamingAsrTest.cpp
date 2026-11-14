#include "private/asr/NativeAsrClient.hpp"
#include <QJsonDocument>
#include <QSignalSpy>
#include <QUuid>
#include <QWebSocketServer>
#include <QtTest>

class MockStreamingAsr : public QWebSocketServer {
public:
    QPointer<QWebSocket> peer;
    QList<QJsonObject> messages;
    QList<QByteArray> chunks;
    int connections = 0;
    MockStreamingAsr()
        : QWebSocketServer(QStringLiteral("offline ASR"), QWebSocketServer::NonSecureMode)
    {
        listen(QHostAddress::LocalHost);
        connect(this, &QWebSocketServer::newConnection, this, [this] {
            peer = nextPendingConnection();
            ++connections;
            connect(peer, &QWebSocket::disconnected, peer, &QObject::deleteLater);
            connect(peer, &QWebSocket::textMessageReceived, this, [this](const QString& text) { messages.append(QJsonDocument::fromJson(text.toUtf8()).object()); });
            connect(peer, &QWebSocket::binaryMessageReceived, this, [this](const QByteArray& bytes) { chunks.append(bytes); });
        });
    }
    QUrl Url() const { return QUrl(QString("ws://127.0.0.1:%1").arg(serverPort())); }
    QString Task() const { return messages.last().value("header").toObject().value("task_id").toString(); }
    void Event(const QString& event, const QJsonObject& payload = { })
    {
        Send({ { "header", QJsonObject { { "event", event }, { "task_id", Task() } } }, { "payload", payload } });
    }
    void Send(const QJsonObject& object) { peer->sendTextMessage(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact))); }
    void Sentence(const QString& text, bool final)
    {
        Event("result-generated", { { "output", QJsonObject { { "sentence", QJsonObject { { "text", text }, { "sentence_end", final } } } } } });
    }
};

// Exercise actual deadline timers without waiting for production timeouts.
class StreamingAsrTest : public QObject {
    Q_OBJECT
private:
    static VoiceInputConfig Config()
    {
        VoiceInputConfig config;
        config.apiKey = "offline-secret";
        config.model = "qwen-audio-3.1-asr-flash-streaming";
        config.endpoint = "wss://dashscope.aliyuncs.com/api-ws/v1/inference";
        return config;
    }
private slots:
    void protocolAudioAndVerbatimResults()
    {
        MockStreamingAsr server;
        QVERIFY(server.isListening());
        NativeAsrClient client(server.Url(), true);
        QSignalSpy success(&client, &NativeAsrClient::Recognized);
        QSignalSpy failure(&client, &NativeAsrClient::Failed);
        const auto pcm = QByteArray::fromHex("0080ff7f").repeated(1000);
        client.Recognize(42, pcm, Config());
        QTRY_COMPARE(server.messages.size(), 1);
        QCOMPARE(server.peer->requestUrl().path(), QUrl(Config().endpoint).path());
        QCOMPARE(server.peer->request().rawHeader("Authorization"), QByteArray("Bearer offline-secret"));
        const auto run = server.messages.first();
        const auto task = server.Task();
        QVERIFY(!QUuid(task).isNull());
        QCOMPARE(QUuid(task).toString(QUuid::WithoutBraces), task);
        QCOMPARE(run.value("header").toObject(), (QJsonObject { { "action", "run-task" }, { "task_id", task }, { "streaming", "duplex" } }));
        QCOMPARE(run.value("payload").toObject(),
            (QJsonObject { { "task_group", "audio" }, { "task", "asr" }, { "function", "recognition" }, { "model", Config().model },
                { "parameters", QJsonObject { { "format", "pcm" }, { "sample_rate", 16000 } } }, { "input", QJsonObject { } } }));
        QTest::qWait(150); // Connected alone must not release binary audio.
        QVERIFY(server.chunks.isEmpty());
        QCOMPARE(server.messages.size(), 1);
        server.Event("task-started");
        server.Event("result-generated", { { "output", QJsonObject { { "sentence", QJsonObject { { "heartbeat", true } } } } } });
        server.Sentence("discard partial", false);
        server.Sentence(QString::fromUtf8(" 你好\n"), true);
        server.Sentence("discard replacement", false);
        server.Sentence(QString::fromUtf8("世界  "), true);
        QTRY_COMPARE(server.messages.size(), 2);
        QCOMPARE(success.size(), 0); // Even final sentences are not completion.
        QCOMPARE(failure.size(), 0);
        const auto finish = server.messages.last();
        QCOMPARE(finish.value("header").toObject(), (QJsonObject { { "action", "finish-task" }, { "task_id", task }, { "streaming", "duplex" } }));
        QCOMPARE(finish.value("payload").toObject(), (QJsonObject { { "input", QJsonObject { } } }));
        QByteArray audio;
        for (qsizetype i = 0; i < server.chunks.size(); ++i) {
            QCOMPARE(server.chunks[i].size(), i == server.chunks.size() - 1 ? 2400 : 3200);
            audio += server.chunks[i];
        }
        QCOMPARE(server.chunks.size(), 4);
        QCOMPARE(audio, pcm + QByteArray(8000, '\0')); // Exactly 250 ms of 16 kHz mono s16le silence.
        server.Event("task-finished");
        QTRY_COMPARE(success.size(), 1);
        QCOMPARE(success[0][0].toULongLong(), quint64(42));
        QCOMPARE(success[0][1].toString(), QString::fromUtf8(" 你好\n世界  "));
        QTest::qWait(30);
        QCOMPARE(failure.size(), 0);
        QCOMPARE(success.size(), 1);
    }
    void failures_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("code");
        for (const auto* kind : { "malformed", "array", "mismatch", "early-finish", "early-result", "bad-sentence", "binary", "disconnect" }) {
            QTest::newRow(kind) << QString(kind) << QString("asr_protocol");
        }
        QTest::newRow("task-failed") << QString("task-failed") << QString("asr_task_failed");
        QTest::newRow("blank-final") << QString("blank-final") << QString("no_speech");
    }
    void failures()
    {
        QFETCH(QString, kind);
        QFETCH(QString, code);
        MockStreamingAsr server;
        NativeAsrClient client(server.Url(), true);
        QSignalSpy success(&client, &NativeAsrClient::Recognized);
        QSignalSpy failure(&client, &NativeAsrClient::Failed);
        client.Recognize(7, QByteArray(2, '\0'), Config());
        QTRY_COMPARE(server.messages.size(), 1);
        if (kind == "malformed") {
            server.peer->sendTextMessage("private-audio offline-secret malformed JSON");
        } else if (kind == "array") {
            server.peer->sendTextMessage("[]");
        } else if (kind == "mismatch") {
            server.Send({ { "header", QJsonObject { { "event", "task-started" }, { "task_id", "private-audio" } } } });
        } else if (kind == "early-finish") {
            server.Event("task-finished");
        } else if (kind == "early-result") {
            server.Sentence("private-audio", true);
        } else if (kind == "bad-sentence") {
            server.Event("task-started");
            server.Event("result-generated", { { "output", QJsonObject { { "sentence", QJsonObject { { "text", "private-audio" }, { "sentence_end", "true" } } } } } });
        } else if (kind == "binary") {
            server.peer->sendBinaryMessage("private-audio offline-secret");
        } else if (kind == "disconnect") {
            server.peer->close();
        } else if (kind == "task-failed") {
            server.Send({ { "header",
                QJsonObject {
                    { "event", "task-failed" }, { "task_id", server.Task() }, { "error_code", "offline-secret" }, { "error_message", "private-audio offline-secret" } } } });
        } else {
            server.Event("task-started");
            server.Sentence(" \n\t", true);
            QTRY_COMPARE(server.messages.size(), 2);
            server.Event("task-finished");
        }
        QTRY_COMPARE(failure.size(), 1);
        QCOMPARE(success.size(), 0);
        QCOMPARE(failure[0][0].toULongLong(), quint64(7));
        QCOMPARE(failure[0][1].toString(), code);
        QVERIFY(!failure[0][2].toString().contains("offline-secret"));
        QVERIFY(!failure[0][2].toString().contains("private-audio"));
        QTest::qWait(30);
        QCOMPARE(failure.size(), 1);
        // Every error must release the transport and allow another request.
        server.messages.clear();
        client.Recognize(8, QByteArray(2, '\0'), Config());
        QTRY_COMPARE(server.messages.size(), 1);
        QCOMPARE(server.connections, 2);
        QCOMPARE(failure.size(), 1);
        client.Cancel();
    }
    void cancellationBusyAndReuse()
    {
        MockStreamingAsr server;
        NativeAsrClient client(server.Url(), true);
        QSignalSpy success(&client, &NativeAsrClient::Recognized);
        QSignalSpy failure(&client, &NativeAsrClient::Failed);
        client.Recognize(10, QByteArray(32000, 'x'), Config());
        QTRY_COMPARE(server.messages.size(), 1);
        const auto oldTask = server.Task();
        client.Recognize(11, QByteArray(2, '\0'), Config());
        QCOMPARE(failure.size(), 1);
        QCOMPARE(failure[0][0].toULongLong(), quint64(11));
        QCOMPARE(failure[0][1].toString(), QString("busy"));
        server.Event("task-started");
        server.Sentence("stale text", true);
        QTRY_VERIFY(!server.chunks.isEmpty());
        client.Cancel();
        client.Cancel();
        QTest::qWait(150);
        const auto chunkCount = server.chunks.size();
        QTest::qWait(150);
        QCOMPARE(server.chunks.size(), chunkCount);
        QCOMPARE(success.size(), 0);
        QCOMPARE(failure.size(), 1);
        server.messages.clear();
        server.chunks.clear();
        client.Recognize(12, QByteArray(2, '\0'), Config());
        QTRY_COMPARE(server.messages.size(), 1);
        QVERIFY(server.Task() != oldTask);
        server.Event("task-started");
        server.Sentence("fresh", true);
        QTRY_COMPARE(server.messages.size(), 2);
        server.Event("task-finished");
        QTRY_COMPARE(success.size(), 1);
        QCOMPARE(success[0][0].toULongLong(), quint64(12));
        QCOMPARE(success[0][1].toString(), QString("fresh"));
        QCOMPARE(failure.size(), 1);
    }
    void deadlines_data()
    {
        QTest::addColumn<bool>("connection");
        QTest::newRow("connection-awaits-task-started") << true;
        QTest::newRow("total-after-task-started") << false;
    }
    void deadlines()
    {
        QFETCH(bool, connection);
        MockStreamingAsr server;
        NativeAsrClient client(server.Url(), true);
        QSignalSpy failure(&client, &NativeAsrClient::Failed);
        QSignalSpy success(&client, &NativeAsrClient::Recognized);
        client.Recognize(20, QByteArray(3200, '\0'), Config());
        QCOMPARE(client._deadline.interval(), 120350); // Audio duration plus trailing pad.
        QCOMPARE(client._connectionDeadline.interval(), 15000);
        QTRY_COMPARE(server.messages.size(), 1);
        QVERIFY(client._connectionDeadline.isActive());
        if (connection) {
            client._connectionDeadline.start(1);
        } else {
            server.Event("task-started");
            QTRY_VERIFY(!client._connectionDeadline.isActive());
            client._deadline.start(1);
        }
        QTRY_COMPARE(failure.size(), 1);
        QCOMPARE(failure[0][0].toULongLong(), quint64(20));
        QCOMPARE(failure[0][1].toString(), QString("asr_timeout"));
        QVERIFY(client._socket.isNull());
        QVERIFY(!client._deadline.isActive());
        QVERIFY(!client._connectionDeadline.isActive());
        QVERIFY(!client._audioTimer.isActive());
        server.messages.clear();
        client.Recognize(21, QByteArray(2, '\0'), Config());
        QTRY_COMPARE(server.messages.size(), 1);
        server.Event("task-started");
        server.Sentence("after timeout", true);
        QTRY_COMPARE(server.messages.size(), 2);
        server.Event("task-finished");
        QTRY_COMPARE(success.size(), 1);
        QCOMPARE(success[0][0].toULongLong(), quint64(21));
        QCOMPARE(failure.size(), 1);
    }
};
QTEST_GUILESS_MAIN(StreamingAsrTest)
#include "StreamingAsrTest.moc"
