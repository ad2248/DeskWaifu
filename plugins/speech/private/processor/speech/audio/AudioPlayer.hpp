#pragma once

#include "processor/speech/SpeechTypes.hpp"
#include <QAudioFormat>
#include <QObject>
#include <QTimer>
#include <memory>

class QAudioSink;
class QIODevice;

// 仅负责 PCM 输出，所有方法均在 Qt 应用线程调用。
class AudioPlayer : public QObject
{
    Q_OBJECT
public:
    explicit AudioPlayer(QObject* parent = nullptr);
    ~AudioPlayer() override;

public slots:
    virtual void Start(SpeechId id, QAudioFormat format);
    virtual void Append(SpeechId id, QByteArray pcm);
    virtual void FinishInput(SpeechId id);
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
