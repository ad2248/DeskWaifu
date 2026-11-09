#include "MiniMaxSpeechSource.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QWebSocket>
#include <algorithm>
#include <cmath>

MiniMaxSpeechSource::MiniMaxSpeechSource(QString apiKey, QObject* parent, QUrl endpoint)
    : QObject(parent), _apiKey(std::move(apiKey)), _endpoint(std::move(endpoint))
{
    _timeout.setSingleShot(true);
    _timeout.setInterval(30000);
    connect(&_timeout, &QTimer::timeout, this, [this] {
        Fail(SpeechErrorCode::Timeout, QStringLiteral("语音服务响应超时"));
    });
}

MiniMaxSpeechSource::~MiniMaxSpeechSource() { Cancel(_id); }

void MiniMaxSpeechSource::SetApiKey(QString apiKey) { _apiKey = std::move(apiKey); }

void MiniMaxSpeechSource::Start(SpeechId id, SpeechRequest request)
{
    Cancel(_id);
    _id = id;
    _request = std::move(request);
    const auto& o = _request.options;
    if (!id || _request.text.trimmed().isEmpty() || _request.text.size() >= 10000 ||
        o.voiceId.isEmpty() || o.model.isEmpty() || !std::isfinite(o.speed) || o.speed < 0.5 || o.speed > 2 ||
        !std::isfinite(o.volume) || o.volume <= 0 || o.volume > 10 || o.pitch < -12 || o.pitch > 12) {
        Fail(SpeechErrorCode::InvalidRequest, QStringLiteral("无效的语音文本或参数")); return;
    }
    if (_apiKey.trimmed().isEmpty()) {
        Fail(SpeechErrorCode::MissingApiKey, QStringLiteral("请先在控制面板中设置 MiniMax API Key")); return;
    }
    _phase = Phase::Connecting;
    _socket.reset(new QWebSocket);
    _socket->setMaxAllowedIncomingMessageSize(2 * 1024 * 1024);
    connect(_socket.get(), &QWebSocket::textMessageReceived, this, &MiniMaxSpeechSource::Receive);
    connect(_socket.get(), &QWebSocket::errorOccurred, this, [this] {
        // 不暴露 errorString 或服务端消息，它们可能包含凭据。
        Fail(SpeechErrorCode::Network, QStringLiteral("语音连接失败"));
    });
    connect(_socket.get(), &QWebSocket::disconnected, this, [this] {
        Fail(SpeechErrorCode::Network, QStringLiteral("语音连接提前关闭"));
    });
    QNetworkRequest connection(_endpoint);
    connection.setRawHeader("Authorization", "Bearer " + _apiKey.toUtf8());
    _timeout.start();
    _socket->open(connection);
}

void MiniMaxSpeechSource::Cancel(SpeechId id)
{
    if (id != _id) return;
    _id = 0;
    _timeout.stop();
    if (_socket) {
        _socket->disconnect(this);
        _socket->abort();
        // 取消操作可能在套接字信号回调中执行。
        _socket.release()->deleteLater();
    }
    _request = {};
}

void MiniMaxSpeechSource::Fail(SpeechErrorCode code, const QString& message)
{
    if (!_id) return;
    const auto id = _id;
    Cancel(id);
    emit Failed(id, {code, message});
}

void MiniMaxSpeechSource::Receive(const QString& text)
{
    if (!_id) return;
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(text.toUtf8(), &parse);
    const auto message = document.object();
    const auto status = message.value(QStringLiteral("base_resp")).toObject().value(QStringLiteral("status_code"));
    if (parse.error != QJsonParseError::NoError || !document.isObject() || !status.isDouble()) {
        Fail(SpeechErrorCode::Protocol, QStringLiteral("无效的语音服务响应")); return;
    }
    if (status.toInt() != 0) {
        Fail(status.toInt() == 1004 ? SpeechErrorCode::Authentication : SpeechErrorCode::Synthesis,
            QStringLiteral("语音服务拒绝请求（代码 %1）").arg(status.toInt())); return;
    }
    _timeout.start();
    const auto event = message.value(QStringLiteral("event")).toString();
    const auto id = _id;
    auto send = [this, id](const QJsonObject& value) {
        if (_id == id && _socket)
            _socket->sendTextMessage(QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact)));
    };
    if (event == QStringLiteral("connected_success") && _phase == Phase::Connecting) {
        _phase = Phase::Starting;
        const auto& o = _request.options;
        send({{"event", "task_start"}, {"model", o.model},
            {"voice_setting", QJsonObject{{"voice_id", o.voiceId}, {"speed", o.speed}, {"vol", o.volume}, {"pitch", o.pitch}}},
            {"audio_setting", QJsonObject{{"format", "pcm"}, {"sample_rate", 24000}, {"channel", 1}}}});
    } else if (event == QStringLiteral("task_started") && _phase == Phase::Starting) {
        _phase = Phase::Receiving;
        QAudioFormat format;
        format.setSampleRate(24000);
        format.setChannelCount(1);
        format.setSampleFormat(QAudioFormat::Int16);
        emit Ready(id, format);
        if (_id != id) return;
        send({{"event", "task_continue"}, {"text", _request.text}});
        send({{"event", "task_finish"}});
    } else if (_phase == Phase::Receiving && event == QStringLiteral("task_continued")) {
        const auto data = message.value(QStringLiteral("data")).toObject();
        const auto encoded = data.value(QStringLiteral("audio"));
        if (!encoded.isUndefined() && !encoded.isString()) {
            Fail(SpeechErrorCode::Protocol, QStringLiteral("无效的 PCM 音频数据")); return;
        }
        const auto encodedText = encoded.toString();
        const auto hex = encodedText.toLatin1();
        if (QString::fromLatin1(hex) != encodedText || hex.size() % 2 || std::any_of(hex.begin(), hex.end(), [](char c) {
            return !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
        })) {
            Fail(SpeechErrorCode::Protocol, QStringLiteral("无效的 PCM 音频编码")); return;
        }
        if (!hex.isEmpty()) emit AudioReceived(id, QByteArray::fromHex(hex));
    } else if (_phase == Phase::Receiving && event == QStringLiteral("task_finished")) {
        Cancel(id);
        emit Completed(id);
    } else if (_phase == Phase::Receiving &&
        (event == QStringLiteral("sentence_start") || event == QStringLiteral("sentence_end"))) {
        // 句子边界不代表音频流结束。
    } else {
        Fail(SpeechErrorCode::Protocol, QStringLiteral("语音服务事件顺序错误"));
    }
}
