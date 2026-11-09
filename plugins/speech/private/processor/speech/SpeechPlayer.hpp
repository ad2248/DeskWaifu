#pragma once

#include "SpeechTypes.hpp"
#include <QStateMachine>

class MiniMaxSpeechSource;
class AudioPlayer;

// 业务入口；语音来源和输出对象必须比此协调器存活更久。
// 与来源及输出对象在同一 Qt 应用线程使用，状态变更由异步状态机驱动。
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
    // 返回当前已接受的播报标识，无活动播报时为 0。
    SpeechId CurrentId() const { return _id; }
    // 更新来源供后续合成连接使用的凭据，不改变当前播放状态。
    void SetApiKey(const QString& key);

public slots:
    // 异步排入播报请求；非空请求替换当前播报，空白文本不打断当前任务。
    void Speak(SpeechRequest request);
    // 异步请求停止；完成清理后通过 Finished 给出 Stopped 原因。
    void Stop();

signals:
    void StateChanged(State state);
    // 状态机接受请求并分配新标识后发出；此时尚不保证音频开始播放。
    void Accepted(SpeechId id);
    void ErrorOccurred(SpeechId id, SpeechError error);
    // 该播报的来源与音频输出均已清理；Completed 表示音频已排空。
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
