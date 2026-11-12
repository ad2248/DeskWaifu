#pragma once
#include "VoiceInputSettings.hpp"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

class NativeAsrClient : public QObject {
    Q_OBJECT
public:
    explicit NativeAsrClient(QObject* parent = nullptr);
    // HTTP is permitted only with this explicit option and a literal loopback host.
    NativeAsrClient(const QUrl& testEndpoint, bool allowLocalHttp, QObject* parent = nullptr);
    ~NativeAsrClient() override;
    virtual void Recognize(quint64 sessionId, const QByteArray& pcm, const VoiceInputConfig& config);
    virtual void Cancel();
signals:
    void Recognized(quint64 sessionId, const QString& text);
    void Failed(quint64 sessionId, const QString& code, const QString& message);

private:
    friend class AsrTest; // Exercise real deadline timers without a two-minute offline test.
    friend class StreamingAsrTest;
    void Fail(const QString& code, const QString& message);
    void RecognizeStreaming(const QUrl& url, const QByteArray& pcm, const VoiceInputConfig& config);
    void HandleStreamingMessage(const QString& message);
    QPointer<QWebSocket> _socket;
    QTimer _audioTimer;
    QByteArray _pcm;
    qsizetype _audioOffset = 0;
    QString _taskId, _text;
    bool _taskStarted = false, _finishSent = false;
    QNetworkAccessManager _network;
    QPointer<QNetworkReply> _reply;
    QTimer _deadline, _connectionDeadline;
    QByteArray _body;
    QUrl _testEndpoint;
    bool _allowLocalHttp = false;
    quint64 _sessionId = 0;
};
