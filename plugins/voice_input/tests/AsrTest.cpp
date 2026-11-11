#include "private/asr/NativeAsrClient.hpp"
#include "private/audio/WavEncoder.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

class MockAsr : public QTcpServer {
public:
    QByteArray response = "{\"text\":\" 你好\\n \"}";
    QByteArray request, payload, extraHeaders;
    int status = 200, requests = 0;
    bool hold = false;
    MockAsr()
    {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto* socket = nextPendingConnection();
                auto data = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, data] {
                    *data += socket->readAll();
                    const auto boundary = data->indexOf("\r\n\r\n");
                    if (boundary < 0) {
                        return;
                    }
                    qint64 length = -1;
                    for (const auto& line : data->first(boundary).split('\n')) {
                        if (line.toLower().startsWith("content-length:")) {
                            length = line.mid(15).trimmed().toLongLong();
                        }
                    }
                    if (length < 0 || data->size() < boundary + 4 + length) {
                        return;
                    }
                    request = data->first(boundary);
                    payload = data->mid(boundary + 4, length);
                    ++requests;
                    socket->disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
                    if (!hold) {
                        socket->write("HTTP/1.1 " + QByteArray::number(status) + " Test\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(response.size())
                            + "\r\n" + extraHeaders + "Connection: close\r\n\r\n" + response);
                        socket->disconnectFromHost();
                    }
                });
            }
        });
    }
    QUrl Url() const { return QUrl(QString("http://127.0.0.1:%1/asr").arg(serverPort())); }
};

class AsrTest : public QObject {
    Q_OBJECT
private:
    static VoiceInputConfig Config()
    {
        VoiceInputConfig c;
        c.apiKey = "offline-key";
        c.endpoint = "https://llm-y3exskfcgxgxzn23.cn-beijing.maas.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation";
        return c;
    }
private slots:
    void protocolAndVerbatimText()
    {
        MockAsr server;
        QVERIFY(server.isListening());
        NativeAsrClient client(server.Url(), true);
        QSignalSpy success(&client, &NativeAsrClient::Recognized);
        QSignalSpy failure(&client, &NativeAsrClient::Failed);
        const auto pcm = QByteArray::fromHex("0080ff7f");
        client.Recognize(42, pcm, Config());
        QTRY_COMPARE(success.size(), 1);
        QCOMPARE(failure.size(), 0);
        QCOMPARE(success[0][0].toULongLong(), quint64(42));
        QCOMPARE(success[0][1].toString(), QString::fromUtf8(" 你好\n "));
        QCOMPARE(server.requests, 1);
        QVERIFY(server.request.startsWith("POST " + QUrl(Config().endpoint).path().toUtf8() + " HTTP/1.1\r\n"));
        const auto headers = server.request.toLower();
        QVERIFY(headers.contains("\r\nauthorization: bearer offline-key"));
        QVERIFY(headers.contains("\r\ncontent-type: application/json"));
        QVERIFY(headers.contains("\r\nx-dashscope-sse: disable"));
        const auto body = QJsonDocument::fromJson(server.payload).object();
        QCOMPARE(body.keys(), QStringList({ "input", "model", "parameters" }));
        QCOMPARE(body.value("model").toString(), Config().model);
        QCOMPARE(body.value("parameters").toObject(), (QJsonObject { { "format", "wav" }, { "sample_rate", "16000" } }));
        const auto messages = body.value("input").toObject().value("messages").toArray();
        QCOMPARE(messages.size(), 1);
        QCOMPARE(messages[0].toObject().value("role").toString(), QString("user"));
        const auto content = messages[0].toObject().value("content").toArray();
        QCOMPARE(content.size(), 1);
        QCOMPARE(content[0].toObject().value("type").toString(), QString("input_audio"));
        const auto data = content[0].toObject().value("input_audio").toObject().value("data").toString().toLatin1();
        QVERIFY(data.startsWith("data:audio/wav;base64,"));
        QString error;
        QCOMPARE(QByteArray::fromBase64(data.mid(22)), WavEncoder::Encode(pcm, error));
    }
    void nativeOnlyModelRoutesCompatibleEndpoint_data()
    {
        QTest::addColumn<QString>("endpoint");
        QTest::addColumn<QByteArray>("response");
        QTest::newRow("user-base-default-model") << VoiceInputConfig { }.endpoint << QByteArray(R"({"text":"native top","output":{"text":"ignored"}})");
        QTest::newRow("generic-base") << QString("https://example.invalid/v1") << QByteArray(R"({"output":{"text":"native fallback"}})");
        QTest::newRow("complete-chat-endpoint") << QString("https://example.invalid/compatible-mode/v1/chat/completions") << QByteArray(R"({"text":"native top"})");
    }
    void nativeOnlyModelRoutesCompatibleEndpoint()
    {
        QFETCH(QString, endpoint);
        QFETCH(QByteArray, response);
        MockAsr server;
        server.response = response;
        NativeAsrClient client(server.Url(), true);
        QSignalSpy success(&client, &NativeAsrClient::Recognized);
        QSignalSpy failed(&client, &NativeAsrClient::Failed);
        VoiceInputConfig config;
        config.apiKey = "offline-key";
        config.endpoint = endpoint;
        const auto pcm = QByteArray::fromHex("0080ff7f");
        client.Recognize(50, pcm, config);
        QTRY_COMPARE(success.size() + failed.size(), 1);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(server.requests, 1);
        QCOMPARE(success[0][0].toULongLong(), quint64(50));
        QCOMPARE(success[0][1].toString(), response.contains("fallback") ? QString("native fallback") : QString("native top"));
        QVERIFY(server.request.startsWith("POST /api/v1/services/aigc/multimodal-generation/generation HTTP/1.1\r\n"));
        const auto headers = server.request.toLower();
        QVERIFY(headers.contains("\r\nauthorization: bearer offline-key"));
        QVERIFY(headers.contains("\r\ncontent-type: application/json"));
        QVERIFY(headers.contains("\r\nx-dashscope-sse: disable"));
        const auto body = QJsonDocument::fromJson(server.payload).object();
        QCOMPARE(body.keys(), QStringList({ "input", "model", "parameters" }));
        QCOMPARE(body.value("model").toString(), QString("qwen-audio-3.0-asr-flash"));
        QCOMPARE(body.value("parameters").toObject(), (QJsonObject { { "format", "wav" }, { "sample_rate", "16000" } }));
        const auto messages = body.value("input").toObject().value("messages").toArray();
        QCOMPARE(messages.size(), 1);
        QCOMPARE(messages[0].toObject().value("role").toString(), QString("user"));
        const auto content = messages[0].toObject().value("content").toArray();
        QCOMPARE(content.size(), 1);
        QCOMPARE(content[0].toObject().value("type").toString(), QString("input_audio"));
        const auto data = content[0].toObject().value("input_audio").toObject().value("data").toString().toLatin1();
        QVERIFY(data.startsWith("data:audio/wav;base64,"));
        QString error;
        QCOMPARE(QByteArray::fromBase64(data.mid(22)), WavEncoder::Encode(pcm, error));
        QCOMPARE(config.endpoint, endpoint); // Resolution never rewrites the persisted/user configuration.
    }
    void compatibleProtocol_data()
    {
        QTest::addColumn<QString>("endpoint");
        QTest::addColumn<QByteArray>("path");
        QTest::newRow("default-compatible-base") << VoiceInputConfig { }.endpoint << QByteArray("/compatible-mode/v1/chat/completions");
        QTest::newRow("generic-base") << QString("https://example.invalid/v1") << QByteArray("/v1/chat/completions");
        QTest::newRow("base-trailing-slash") << QString("https://example.invalid/v1/") << QByteArray("/v1/chat/completions");
        QTest::newRow("full-no-duplicate") << QString("https://example.invalid/compatible-mode/v1/chat/completions") << QByteArray("/compatible-mode/v1/chat/completions");
    }
    void compatibleProtocol()
    {
        QFETCH(QString, endpoint);
        QFETCH(QByteArray, path);
        MockAsr server;
        server.response = R"({"text":"ignored native text","choices":[{"message":{"content":" 兼容结果\n "}}]})";
        NativeAsrClient client(server.Url(), true);
        QSignalSpy success(&client, &NativeAsrClient::Recognized);
        QSignalSpy failed(&client, &NativeAsrClient::Failed);
        auto config = Config();
        config.endpoint = endpoint;
        config.model = "qwen3-asr-flash";
        const auto pcm = QByteArray::fromHex("0080ff7f");
        client.Recognize(51, pcm, config);
        QTRY_COMPARE(success.size() + failed.size(), 1);
        QCOMPARE(failed.size(), 0);
        QCOMPARE(success[0][0].toULongLong(), quint64(51));
        QCOMPARE(success[0][1].toString(), QString::fromUtf8(" 兼容结果\n "));
        QVERIFY(server.request.startsWith("POST " + path + " HTTP/1.1\r\n"));
        const auto headers = server.request.toLower();
        QVERIFY(headers.contains("\r\nauthorization: bearer offline-key"));
        QVERIFY(headers.contains("\r\ncontent-type: application/json"));
        QVERIFY(!headers.contains("x-dashscope-sse:"));
        const auto body = QJsonDocument::fromJson(server.payload).object();
        QCOMPARE(body.keys(), QStringList({ "messages", "model", "stream" }));
        QCOMPARE(body.value("model").toString(), config.model);
        QVERIFY(body.value("stream").isBool());
        QVERIFY(!body.value("stream").toBool());
        const auto messages = body.value("messages").toArray();
        QCOMPARE(messages.size(), 1);
        QCOMPARE(messages[0].toObject().value("role").toString(), QString("user"));
        const auto content = messages[0].toObject().value("content").toArray();
        QCOMPARE(content.size(), 1);
        QCOMPARE(content[0].toObject().value("type").toString(), QString("input_audio"));
        const auto data = content[0].toObject().value("input_audio").toObject().value("data").toString().toLatin1();
        QVERIFY(data.startsWith("data:audio/wav;base64,"));
        QString error;
        QCOMPARE(QByteArray::fromBase64(data.mid(22)), WavEncoder::Encode(pcm, error));
    }
    void compatibleResponseValidation_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<QString>("code");
        QTest::newRow("missing-choices") << QByteArray(R"({"text":"native must not be accepted"})") << QString("asr_protocol");
        QTest::newRow("empty-choices") << QByteArray(R"({"choices":[]})") << QString("asr_protocol");
        QTest::newRow("nonstring-content") << QByteArray(R"({"choices":[{"message":{"content":[]}}]})") << QString("asr_protocol");
        QTest::newRow("blank-content") << QByteArray(R"({"choices":[{"message":{"content":" \n "}}]})") << QString("no_speech");
    }
    void compatibleResponseValidation()
    {
        QFETCH(QByteArray, body);
        QFETCH(QString, code);
        MockAsr server;
        server.response = body;
        NativeAsrClient client(server.Url(), true);
        QSignalSpy failed(&client, &NativeAsrClient::Failed);
        QSignalSpy success(&client, &NativeAsrClient::Recognized);
        auto config = Config();
        config.endpoint = VoiceInputConfig { }.endpoint;
        config.model = "test-compatible";
        client.Recognize(52, QByteArray(2, '\0'), config);
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(success.size(), 0);
        QCOMPARE(failed[0][1].toString(), code);
    }
    void responseSemantics_data()
    {
        QTest::addColumn<QByteArray>("body");
        QTest::addColumn<int>("status");
        QTest::addColumn<QString>("text");
        QTest::addColumn<QString>("code");
        QTest::newRow("priority") << QByteArray(R"({"text":"top","output":{"text":"fallback"}})") << 200 << QString("top") << QString();
        QTest::newRow("fallback") << QByteArray(R"({"output":{"text":"fallback"}})") << 200 << QString("fallback") << QString();
        QTest::newRow("empty-top") << QByteArray(R"({"text":"","output":{"text":"fallback"}})") << 200 << QString() << QString("no_speech");
        QTest::newRow("whitespace") << QByteArray(R"({"text":" \n\t"})") << 200 << QString() << QString("no_speech");
        QTest::newRow("missing") << QByteArray("{}") << 200 << QString() << QString("asr_protocol");
        QTest::newRow("invalid") << QByteArray("private invalid JSON") << 200 << QString() << QString("asr_protocol");
        for (const int status : { 301, 401, 429, 503 }) {
            QTest::newRow(qPrintable(QString::number(status))) << QByteArray("private-secret") << status << QString() << QString("asr_http");
        }
        QTest::newRow("oversized") << QByteArray(1024 * 1024 + 1, 'x') << 200 << QString() << QString("asr_response_too_large");
    }
    void responseSemantics()
    {
        QFETCH(QByteArray, body);
        QFETCH(int, status);
        QFETCH(QString, text);
        QFETCH(QString, code);
        MockAsr server;
        server.response = body;
        server.status = status;
        NativeAsrClient client(server.Url(), true);
        QSignalSpy success(&client, &NativeAsrClient::Recognized);
        QSignalSpy failure(&client, &NativeAsrClient::Failed);
        client.Recognize(7, QByteArray(2, '\0'), Config());
        QTRY_COMPARE(success.size() + failure.size(), 1);
        if (code.isEmpty()) {
            QCOMPARE(success.size(), 1);
            QCOMPARE(success[0][1].toString(), text);
        } else {
            QCOMPARE(failure.size(), 1);
            QCOMPARE(failure[0][1].toString(), code);
            QVERIFY(!failure[0][2].toString().contains("private"));
        }
        QCOMPARE(server.requests, 1);
    }
    void redirectNeverForwardsAuthorization()
    {
        MockAsr target, redirect;
        redirect.status = 302;
        redirect.extraHeaders = "Location: " + target.Url().toEncoded() + "\r\n";
        NativeAsrClient client(redirect.Url(), true);
        QSignalSpy failed(&client, &NativeAsrClient::Failed);
        client.Recognize(5, QByteArray(2, '\0'), Config());
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed[0][1].toString(), QString("asr_http"));
        QCOMPARE(redirect.requests, 1);
        QCOMPARE(target.requests, 0);
    }
    void deadlinesAbortAndPermitNextRequest_data()
    {
        QTest::addColumn<bool>("connection");
        QTest::newRow("total") << false;
        QTest::newRow("connection") << true;
    }
    void deadlinesAbortAndPermitNextRequest()
    {
        QFETCH(bool, connection);
        MockAsr server;
        server.hold = true;
        NativeAsrClient client(server.Url(), true);
        QSignalSpy failed(&client, &NativeAsrClient::Failed);
        QSignalSpy success(&client, &NativeAsrClient::Recognized);
        client.Recognize(6, QByteArray(2, '\0'), Config());
        QCOMPARE(client._deadline.interval(), 120000);
        QCOMPARE(client._connectionDeadline.interval(), 15000);
        QTRY_COMPARE(server.requests, 1);
        if (connection) {
            client._connectionDeadline.start(1);
        } else {
            client._deadline.start(1);
        }
        QTRY_COMPARE(failed.size(), 1);
        QCOMPARE(failed[0][0].toULongLong(), quint64(6));
        QCOMPARE(failed[0][1].toString(), QString("asr_timeout"));
        QVERIFY(client._reply.isNull());
        QVERIFY(!client._deadline.isActive());
        server.hold = false;
        client.Recognize(7, QByteArray(2, '\0'), Config());
        QTRY_COMPARE(success.size(), 1);
        QCOMPARE(failed.size(), 1);
    }
    void cancelAndExplicitHttpOptIn()
    {
        MockAsr server;
        server.hold = true;
        NativeAsrClient forbidden(server.Url(), false);
        QSignalSpy denied(&forbidden, &NativeAsrClient::Failed);
        forbidden.Recognize(1, QByteArray(2, '\0'), Config());
        QCOMPARE(denied.size(), 1);
        QCOMPARE(server.requests, 0);
        NativeAsrClient client(server.Url(), true);
        QSignalSpy success(&client, &NativeAsrClient::Recognized);
        QSignalSpy failure(&client, &NativeAsrClient::Failed);
        client.Recognize(2, QByteArray(2, '\0'), Config());
        QTRY_COMPARE(server.requests, 1);
        client.Cancel();
        server.hold = false;
        client.Recognize(3, QByteArray(2, '\0'), Config());
        QTRY_COMPARE(success.size(), 1);
        QCOMPARE(success[0][0].toULongLong(), quint64(3));
        QCOMPARE(failure.size(), 0);
    }
};
QTEST_GUILESS_MAIN(AsrTest)
#include "AsrTest.moc"
