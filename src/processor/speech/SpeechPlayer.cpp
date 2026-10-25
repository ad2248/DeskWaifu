#include "SpeechPlayer.hpp"
#include "audio/AudioPlayer.hpp"
#include "source/MiniMaxSpeechSource.hpp"
#include <QAbstractTransition>
#include <QCoreApplication>
#include <functional>

namespace {
const auto SpeechEventType = static_cast<QEvent::Type>(QEvent::registerEventType());
enum class Kind { Speak, Stop, Started, Completed, Drained, Failed };
struct SpeechEvent : QEvent
{
    explicit SpeechEvent(Kind kind, SpeechId id = 0) : QEvent(SpeechEventType), kind(kind), id(id) {}
    Kind kind;
    SpeechId id;
    SpeechRequest request;
    SpeechError error{SpeechErrorCode::Protocol, {}};
};
class SpeechTransition : public QAbstractTransition
{
public:
    SpeechTransition(QState* source, QState* target, Kind kind,
        std::function<bool(const SpeechEvent&)> guard, std::function<void(const SpeechEvent&)> action)
        : QAbstractTransition(source), _kind(kind), _guard(std::move(guard)), _action(std::move(action))
    { setTargetState(target); }
protected:
    bool eventTest(QEvent* event) override
    {
        if (event->type() != SpeechEventType) return false;
        const auto& speech = *static_cast<SpeechEvent*>(event);
        return speech.kind == _kind && _guard(speech);
    }
    void onTransition(QEvent* event) override { _action(*static_cast<SpeechEvent*>(event)); }
private:
    Kind _kind;
    std::function<bool(const SpeechEvent&)> _guard;
    std::function<void(const SpeechEvent&)> _action;
};
}

SpeechPlayer::SpeechPlayer(MiniMaxSpeechSource& source, AudioPlayer& audio, QObject* parent)
    : QObject(parent), _source(source), _audio(audio)
{
    qRegisterMetaType<SpeechRequest>();
    qRegisterMetaType<SpeechError>();
    auto* idle = new QState(&_machine);
    auto* preparing = new QState(&_machine);
    auto* playing = new QState(&_machine);
    auto* draining = new QState(&_machine);
    _machine.setInitialState(idle);
    auto active = [this](const SpeechEvent& e) { return _id && e.id == _id; };
    auto any = [](const SpeechEvent&) { return true; };
    auto nothing = [](const SpeechEvent&) {};
    const std::pair<QState*, State> states[] = {{idle, State::Idle}, {preparing, State::Preparing},
        {playing, State::Playing}, {draining, State::Draining}};
    for (const auto& entry : states) {
        connect(entry.first, &QState::entered, this, [this, entry] {
            _state = entry.second;
            emit StateChanged(_state);
        });
        new SpeechTransition(entry.first, preparing, Kind::Speak, any, [this](const SpeechEvent& e) {
            End(FinishReason::Replaced);
            _id = ++_nextId;
            emit Accepted(_id);
            _source.Start(_id, e.request);
        });
        if (entry.first == idle) continue;
        new SpeechTransition(entry.first, idle, Kind::Stop, any, [this](const SpeechEvent&) { End(FinishReason::Stopped); });
        new SpeechTransition(entry.first, idle, Kind::Failed, active, [this](const SpeechEvent& e) {
            emit ErrorOccurred(e.id, e.error);
            End(FinishReason::Failed);
        });
    }
    new SpeechTransition(preparing, playing, Kind::Started, active, nothing);
    for (auto* state : {preparing, playing})
        new SpeechTransition(state, draining, Kind::Completed, active, [this](const SpeechEvent&) { _audio.FinishInput(_id); });
    new SpeechTransition(draining, idle, Kind::Drained, active, [this](const SpeechEvent&) { End(FinishReason::Completed); });

    connect(&_source, &MiniMaxSpeechSource::Ready, this, [this](SpeechId id, QAudioFormat format) {
        if (id == _id && id) _audio.Start(id, format);
    });
    connect(&_source, &MiniMaxSpeechSource::AudioReceived, this, [this](SpeechId id, QByteArray pcm) {
        if (id == _id && id) _audio.Append(id, std::move(pcm));
    });
    auto post = [this](Kind kind, SpeechId id) { _machine.postEvent(new SpeechEvent(kind, id)); };
    connect(&_source, &MiniMaxSpeechSource::Completed, this, [post](SpeechId id) { post(Kind::Completed, id); });
    connect(&_audio, &AudioPlayer::PlaybackStarted, this, [post](SpeechId id) { post(Kind::Started, id); });
    connect(&_audio, &AudioPlayer::Drained, this, [post](SpeechId id) { post(Kind::Drained, id); });
    auto failed = [this](SpeechId id, SpeechError error) {
        auto* event = new SpeechEvent(Kind::Failed, id);
        event->error = std::move(error);
        _machine.postEvent(event);
    };
    connect(&_source, &MiniMaxSpeechSource::Failed, this, failed);
    connect(&_audio, &AudioPlayer::Failed, this, failed);
    _machine.start();
    // 只初始化当前状态机，不运行嵌套应用事件循环。
    QCoreApplication::sendPostedEvents(&_machine, QEvent::MetaCall);
}

SpeechPlayer::~SpeechPlayer()
{
    _machine.stop();
    End(FinishReason::Stopped);
}

void SpeechPlayer::Speak(SpeechRequest request)
{
    // 空输入不能打断当前播报。
    if (request.text.trimmed().isEmpty()) return;
    auto* event = new SpeechEvent(Kind::Speak);
    event->request = std::move(request);
    _machine.postEvent(event);
}

void SpeechPlayer::Stop() { _machine.postEvent(new SpeechEvent(Kind::Stop)); }

void SpeechPlayer::End(FinishReason reason)
{
    if (!_id) return;
    const auto id = _id;
    _id = 0; // 在任一组件发出回调之前使标识失效。
    _source.Cancel(id);
    _audio.Stop(id);
    emit Finished(id, reason);
}
