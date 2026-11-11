#pragma once

#include "processor/speech/SpeechTypes.hpp"
#include <QAudioFormat>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <memory>

class QWebSocket;

// 通过 MiniMax WebSocket 协议获取单任务 PCM 流，不负责本地播放。
// 在所属 Qt 事件循环线程串行使用；持有网络连接与超时计时器。
class MiniMaxSpeechSource : public QObject {
    Q_OBJECT
public:
    explicit MiniMaxSpeechSource(QString apiKey, QObject* parent = nullptr, QUrl endpoint = QUrl(QStringLiteral("wss://api.minimax.cn/ws/v1/t2a_v2_bidi")));
    ~MiniMaxSpeechSource() override;
    // 更新后续连接使用的凭据，不重启当前任务。
    void SetApiKey(QString apiKey);

public slots:
    // 取消旧任务并校验新输入，随后异步连接；id 须非零，失败通过 Failed 报告。
    virtual void Start(SpeechId id, SpeechRequest request);
    // 仅取消匹配任务并关闭连接，不发出 Completed 或 Failed。
    virtual void Cancel(SpeechId id);

signals:
    void Ready(SpeechId id, QAudioFormat format);
    void AudioReceived(SpeechId id, QByteArray pcm);
    // 合成流已结束，不表示本地音频输出已播放完毕。
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
