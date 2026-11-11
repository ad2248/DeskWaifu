#pragma once

#include "processor/speech/SpeechTypes.hpp"
#include <QAudioFormat>
#include <QObject>
#include <QTimer>
#include <memory>

class QAudioSink;
class QIODevice;

// 仅负责 PCM 输出，所有方法均在 Qt 应用线程调用。
class AudioPlayer : public QObject {
    Q_OBJECT
public:
    explicit AudioPlayer(QObject* parent = nullptr);
    ~AudioPlayer() override;

public slots:
    // 清理旧输出并为 id 打开默认音频设备；格式不支持或打开失败时发出 Failed。
    virtual void Start(SpeechId id, QAudioFormat format);
    // 缓存并推进当前播报的 PCM；过期标识或已结束的输入被忽略。
    // 待播放数据超过 8 MiB 时终止并报告缓冲区溢出。
    virtual void Append(SpeechId id, QByteArray pcm);
    // 标记当前输入结束；完整音频播放完毕后发出 Drained，不立即截断输出。
    virtual void FinishInput(SpeechId id);
    // 匹配当前标识时立即丢弃缓冲并关闭输出，不发出 Drained。
    virtual void Stop(SpeechId id);

signals:
    void PlaybackStarted(SpeechId id);
    void Drained(SpeechId id);
    void Failed(SpeechId id, SpeechError error);

private:
    void Pump();
    void Fail(SpeechErrorCode code, const QString& message);
    std::unique_ptr<QAudioSink> _sink;
    QIODevice* _output = nullptr;
    QTimer _pump;
    QByteArray _pending;
    QAudioFormat _format;
    SpeechId _id = 0;
    bool _finished = false;
    bool _started = false;
};
