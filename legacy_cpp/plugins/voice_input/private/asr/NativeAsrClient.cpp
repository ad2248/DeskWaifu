#include "NativeAsrClient.hpp"
#include "private/audio/WavEncoder.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkRequest>
#include <QUuid>

namespace {
constexpr qsizetype MaxResponse = 1024 * 1024;
}
NativeAsrClient::NativeAsrClient(QObject* parent)
    : NativeAsrClient(QUrl(), false, parent)
{
}
NativeAsrClient::NativeAsrClient(const QUrl& endpoint, bool allowLocalHttp, QObject* parent)
    : QObject(parent)
    , _testEndpoint(endpoint)
    , _allowLocalHttp(allowLocalHttp)
{
    _deadline.setSingleShot(true);
    _connectionDeadline.setSingleShot(true);
    connect(&_deadline, &QTimer::timeout, this, [this] { Fail("asr_timeout", "Speech recognition deadline exceeded"); });
    connect(&_connectionDeadline, &QTimer::timeout, this, [this] { Fail("asr_timeout", "Speech recognition connection timed out"); });
}
NativeAsrClient::~NativeAsrClient() { NativeAsrClient::Cancel(); }
void NativeAsrClient::Cancel()
{
    _deadline.stop();
    _connectionDeadline.stop();
    _body.clear();
    _audioTimer.stop();
    _pcm.clear();
    _text.clear();
    if (_socket) {
        auto* socket = _socket.data();
        _socket = nullptr;
        socket->disconnect(this);
        _audioTimer.disconnect(socket);
        socket->abort();
        socket->deleteLater();
    }
    if (_reply) {
        auto* reply = _reply.data();
        _reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
}
void NativeAsrClient::Fail(const QString& code, const QString& message)
{
    const auto id = _sessionId;
    Cancel();
    emit Failed(id, code, message);
}
void NativeAsrClient::Recognize(quint64 sessionId, const QByteArray& pcm, const VoiceInputConfig& config)
{
    if (_reply || _socket) {
        emit Failed(sessionId, "busy", "Speech recognition is busy");
        return;
    }
    _sessionId = sessionId;
    QUrl url(config.endpoint, QUrl::StrictMode);
    QString path = url.path();
    while (path.endsWith('/')) {
        path.chop(1);
    }
    const bool compatibleBase = path.endsWith("/v1");
    const bool compatiblePath = compatibleBase || path.endsWith("/chat/completions");
    // This model is native-only even when listed by the compatible models API.
    // Resolve before sending, on the configured origin; never retry audio with another protocol.
    const bool nativeOnlyModel = config.model == QStringLiteral("qwen-audio-3.0-asr-flash");
    const bool compatible = compatiblePath && !nativeOnlyModel;
    if (compatiblePath && nativeOnlyModel) {
        url.setPath(QStringLiteral("/api/v1/services/aigc/multimodal-generation/generation"));
    } else if (compatibleBase) {
        url.setPath(path + QStringLiteral("/chat/completions"));
    }
    // Test injection changes only transport/destination, never protocol or the resolved API path.
    if (!_testEndpoint.isEmpty()) {
        url.setScheme(_testEndpoint.scheme());
        url.setHost(_testEndpoint.host());
        url.setPort(_testEndpoint.port());
    }
    const bool streaming = QUrl(config.endpoint).scheme() == "wss";
    const bool localHttp = _allowLocalHttp && !_testEndpoint.isEmpty() && (url.scheme() == "http" || (streaming && url.scheme() == "ws"))
        && (url.host() == "127.0.0.1" || url.host() == "::1" || url.host() == "localhost");
    bool validKey = !config.apiKey.isEmpty() && config.apiKey.size() <= 4096;
    for (const auto c : config.apiKey) {
        if (c.category() == QChar::Other_Control) {
            validKey = false;
        }
    }
    if (!validKey || !url.isValid() || (url.scheme() != "https" && url.scheme() != "wss" && !localHttp) || url.host().isEmpty() || url.authority().contains('@')
        || url.hasFragment()) {
        Fail("not_configured", "Invalid speech recognition configuration");
        return;
    }
    QString error;
    if (pcm.isEmpty()) {
        Fail("no_audio", "No audio was recorded");
        return;
    }
    const auto wav = WavEncoder::Encode(pcm, error);
    if (wav.isEmpty()) {
        Fail("invalid_audio", "Invalid audio data");
        return;
    }
    if (streaming) {
        RecognizeStreaming(url, wav.mid(44), config);
        return;
    }
    const QJsonObject audio { { "type", "input_audio" }, { "input_audio", QJsonObject { { "data", QString::fromLatin1("data:audio/wav;base64," + wav.toBase64()) } } } };
    const QJsonArray messages { QJsonObject { { "role", "user" }, { "content", QJsonArray { audio } } } };
    const QJsonObject body = compatible ? QJsonObject { { "model", config.model }, { "messages", messages }, { "stream", false } }
                                        : QJsonObject { { "model", config.model }, { "input", QJsonObject { { "messages", messages } } },
                                              { "parameters", QJsonObject { { "format", "wav" }, { "sample_rate", "16000" } } } };
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", "Bearer " + config.apiKey.toUtf8());
    if (!compatible) {
        request.setRawHeader("X-DashScope-SSE", "disable");
    }
    _body.clear();
    _reply = _network.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    const auto reply = _reply;
    // Qt's default read buffer is unlimited; limit it in addition to our accumulated response.
    reply->setReadBufferSize(MaxResponse + 1);
    _deadline.start(120000);
    _connectionDeadline.start(15000);
#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    connect(reply, &QNetworkReply::requestSent, this, [this] { _connectionDeadline.stop(); });
#endif
    connect(reply, &QNetworkReply::encrypted, this, [this] { _connectionDeadline.stop(); });
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply] {
        if (_reply != reply) {
            return;
        }
        _connectionDeadline.stop();
        if (reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > MaxResponse) {
            Fail("asr_response_too_large", "Speech recognition response exceeded limit");
        }
    });
    connect(reply, &QIODevice::readyRead, this, [this, reply] {
        if (_reply != reply) {
            return;
        }
        _body += reply->read(MaxResponse + 1 - _body.size());
        if (_body.size() > MaxResponse) {
            Fail("asr_response_too_large", "Speech recognition response exceeded limit");
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, compatible] {
        if (_reply != reply) {
            return;
        }
        _body += reply->read(MaxResponse + 1 - _body.size());
        const auto body = _body;
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto networkError = reply->error();
        if (body.size() > MaxResponse) {
            Fail("asr_response_too_large", "Speech recognition response exceeded limit");
            return;
        }
        if (status < 200 || status >= 300) {
            Fail("asr_http", status ? QStringLiteral("Speech recognition HTTP status %1").arg(status) : QStringLiteral("Speech recognition network failure"));
            return;
        }
        if (networkError != QNetworkReply::NoError) {
            Fail("asr_network", "Speech recognition network failure");
            return;
        }
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            Fail("asr_protocol", "Invalid speech recognition response");
            return;
        }
        const auto object = document.object();
        const auto top = object.value("text");
        const auto choices = object.value("choices").toArray();
        const auto value = compatible ? (choices.isEmpty() ? QJsonValue() : choices.at(0)).toObject().value("message").toObject().value("content")
                                      : (top.isString() ? top : object.value("output").toObject().value("text"));
        if (!value.isString()) {
            Fail("asr_protocol", "Missing speech recognition text");
            return;
        }
        const auto text = value.toString();
        if (text.trimmed().isEmpty()) {
            Fail("no_speech", "No speech detected");
            return;
        }
        const auto id = _sessionId;
        Cancel();
        emit Recognized(id, text);
    });
}

void NativeAsrClient::RecognizeStreaming(const QUrl& url, const QByteArray& pcm, const VoiceInputConfig& config)
{
    _pcm = pcm;
    _audioOffset = 0;
    _text.clear();
    _taskStarted = _finishSent = false;
    _taskId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto* socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    _socket = socket;
    socket->setMaxAllowedIncomingFrameSize(MaxResponse);
    socket->setMaxAllowedIncomingMessageSize(MaxResponse);
    connect(socket, &QWebSocket::connected, this, [this, socket, config] {
        const QJsonObject request { { "header", QJsonObject { { "action", "run-task" }, { "task_id", _taskId }, { "streaming", "duplex" } } },
            { "payload",
                QJsonObject { { "task_group", "audio" }, { "task", "asr" }, { "function", "recognition" }, { "model", config.model },
                    { "parameters", QJsonObject { { "format", "pcm" }, { "sample_rate", 16000 } } }, { "input", QJsonObject { } } } } };
        socket->sendTextMessage(QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact)));
    });
    connect(socket, &QWebSocket::textMessageReceived, this, &NativeAsrClient::HandleStreamingMessage);
    connect(socket, &QWebSocket::binaryMessageReceived, this, [this] { Fail("asr_protocol", "Unexpected speech recognition binary response"); });
    connect(socket, &QWebSocket::errorOccurred, this, [this] { Fail("asr_network", "Speech recognition WebSocket failure"); });
    connect(socket, &QWebSocket::disconnected, this, [this] { Fail("asr_protocol", "Speech recognition connection closed before task completion"); });
    _audioTimer.setInterval(100);
    // A new socket owns the timer connection so cancellation cannot leave old send callbacks.
    connect(&_audioTimer, &QTimer::timeout, socket, [this, socket] {
        if (_audioOffset < _pcm.size()) {
            socket->sendBinaryMessage(_pcm.mid(_audioOffset, 3200));
            _audioOffset += 3200;
        } else {
            _audioTimer.stop();
            _pcm.clear();
            _finishSent = true;
            const QJsonObject request { { "header", QJsonObject { { "action", "finish-task" }, { "task_id", _taskId }, { "streaming", "duplex" } } },
                { "payload", QJsonObject { { "input", QJsonObject { } } } } };
            socket->sendTextMessage(QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact)));
        }
    });
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Authorization", "Bearer " + config.apiKey.toUtf8());
    _deadline.start(120000 + int(pcm.size() / 32));
    _connectionDeadline.start(15000);
    socket->open(request);
}

void NativeAsrClient::HandleStreamingMessage(const QString& message)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(message.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        Fail("asr_protocol", "Invalid speech recognition event");
        return;
    }
    const auto object = document.object();
    const auto header = object.value("header").toObject();
    if (header.value("task_id").toString() != _taskId) {
        Fail("asr_protocol", "Mismatched speech recognition task");
        return;
    }
    const auto event = header.value("event").toString();
    if (event == "task-failed") {
        // Do not echo server diagnostics, which may include credentials or audio content.
        Fail("asr_task_failed", "Speech recognition task failed");
    } else if (event == "task-started" && !_taskStarted) {
        _taskStarted = true;
        _connectionDeadline.stop();
        _audioTimer.start();
    } else if (event == "result-generated" && _taskStarted) {
        const auto sentence = object.value("payload").toObject().value("output").toObject().value("sentence").toObject();
        if (sentence.value("heartbeat").toBool()) {
            return;
        }
        if (!sentence.value("text").isString() || !sentence.value("sentence_end").isBool()) {
            Fail("asr_protocol", "Invalid speech recognition sentence");
            return;
        }
        // Partial hypotheses replace one another; only finalized sentences belong in the clipboard.
        if (sentence.value("sentence_end").toBool()) {
            _text += sentence.value("text").toString();
            if (_text.toUtf8().size() > MaxResponse) {
                Fail("asr_response_too_large", "Speech recognition response exceeded limit");
            }
        }
    } else if (event == "task-finished" && _finishSent) {
        const auto text = _text;
        const auto id = _sessionId;
        if (text.trimmed().isEmpty()) {
            Fail("no_speech", "No speech detected");
            return;
        }
        Cancel();
        emit Recognized(id, text);
    } else {
        Fail("asr_protocol", "Unexpected speech recognition event");
    }
}
