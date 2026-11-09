#pragma once

#include "SpeechTypes.hpp"
#include <QStateMachine>

class MiniMaxSpeechSource;
class AudioPlayer;

// 业务入口；语音来源和输出对象必须比此协调器存活更久。
class SpeechPlayer : public QObject
{
    Q_OBJECT
public:
    enum class State { Idle, Preparing, Playing, Draining };
    Q_ENUM(State)
    enum class FinishReason { Completed, Stopped, Replaced, Failed };
    Q_ENUM(FinishReason)

    SpeechPlayer(MiniMaxSpeechSource& source, AudioPlayer& audio, QObject* parent = nullptr);
    ~SpeechPlayer() override;
    State CurrentState() const { return _state; }
    SpeechId CurrentId() const { return _id; }
    void SetApiKey(const QString& key);

public slots:
    void Speak(SpeechRequest request);
    void Stop();

signals:
    void StateChanged(State state);
    void Accepted(SpeechId id);
    void ErrorOccurred(SpeechId id, SpeechError error);
    void Finished(SpeechId id, FinishReason reason);

private:
    void End(FinishReason reason);
    MiniMaxSpeechSource& _source;
    AudioPlayer& _audio;
    QStateMachine _machine;
    State _state = State::Idle;
    SpeechId _id = 0;
    SpeechId _nextId = 0;
};
