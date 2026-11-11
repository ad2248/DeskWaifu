#include "NativeAsrClient.hpp"
#include "private/audio/WavEncoder.hpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkRequest>

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
    if (_reply) {
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
    const bool localHttp = _allowLocalHttp && !_testEndpoint.isEmpty() && url.scheme() == "http" && (url.host() == "127.0.0.1" || url.host() == "::1" || url.host() == "localhost");
    bool validKey = !config.apiKey.isEmpty() && config.apiKey.size() <= 4096;
    for (const auto c : config.apiKey) {
        if (c.category() == QChar::Other_Control) {
            validKey = false;
        }
    }
    if (!validKey || !url.isValid() || (url.scheme() != "https" && !localHttp) || url.host().isEmpty() || url.authority().contains('@') || url.hasFragment()) {
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
