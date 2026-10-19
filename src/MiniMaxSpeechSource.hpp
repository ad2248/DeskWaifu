#pragma once

#include "SpeechTypes.hpp"
#include <QAudioFormat>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <memory>

class QWebSocket;

class MiniMaxSpeechSource : public QObject
{
    Q_OBJECT
public:
    explicit MiniMaxSpeechSource(QString apiKey, QObject* parent = nullptr,
        QUrl endpoint = QUrl(QStringLiteral("wss://api.minimax.cn/ws/v1/t2a_v2_bidi")));
    ~MiniMaxSpeechSource() override;
    void SetApiKey(QString apiKey);

public slots:
    virtual void Start(SpeechId id, SpeechRequest request);
    virtual void Cancel(SpeechId id);

signals:
    void Ready(SpeechId id, QAudioFormat format);
    void AudioReceived(SpeechId id, QByteArray pcm);
    void Completed(SpeechId id);
    void Failed(SpeechId id, SpeechError error);

private:
    void Receive(const QString& text);
    void Fail(SpeechErrorCode code, const QString& message);
    QString _apiKey;
    QUrl _endpoint;
    std::unique_ptr<QWebSocket> _socket;
    QTimer _timeout;
    SpeechId _id = 0;
    SpeechRequest _request;
    enum class Phase { Connecting, Starting, Receiving };
    Phase _phase = Phase::Connecting;
};
