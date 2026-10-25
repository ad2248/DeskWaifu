#include "SpeechProcessor.hpp"
#include "processor/Events.hpp"

SpeechProcessor::SpeechProcessor(ProcessorEventLoop& loop, SpeechPlayer& player, std::function<void()> prepare)
    : Processor(loop), _player(player), _prepare(std::move(prepare))
{
    connect(&_player, &SpeechPlayer::Accepted, this, [this](SpeechId speech) {
        if (_busy && !_speech) _speech = speech;
    });
    connect(&_player, &SpeechPlayer::ErrorOccurred, this, [this](SpeechId speech, const SpeechError& error) {
        if (_busy && speech == _speech) _error = error.message;
    });
    connect(&_player, &SpeechPlayer::Finished, this, [this](SpeechId speech, SpeechPlayer::FinishReason reason) {
        if (!_busy || speech != _speech) return;
        auto* receiver = _requester;
        const bool completed = !_cancelled && reason == SpeechPlayer::FinishReason::Completed;
        const auto error = _error.isEmpty() ? QStringLiteral("speech cancelled or replaced") : _error;
        _busy = false; _speech = 0; _requester = nullptr;
        Send(completed ? ProcessorEvents::Spoken : ProcessorEvents::Failed, receiver,
            completed ? QVariantMap{} : QVariantMap{{"error", error}});
        if (_replacement) {
            auto next = std::move(*_replacement);
            _replacement.reset();
            _loop.Post(std::move(next));
        }
    });
}

SpeechProcessor::~SpeechProcessor()
{
    _player.disconnect(this);
    if (_busy) _player.Stop();
}

bool SpeechProcessor::HandleEvent(const ProcessorEvent& event)
{
    using namespace ProcessorEvents;
    if (event.type == Cancel && _busy && event.sender == _requester) {
        _cancelled = true; _player.Stop(); return true;
    }
    if (event.type != Speak) return false;
    const auto text = event.data.value("text").toString();
    if (text.trimmed().isEmpty()) { Send(Failed, event.sender, {{"error", "speech text is empty"}}); return true; }
    if (_busy) {
        if (event.data.value("replace").toBool()) {
            if (_replacement) Send(Failed, _replacement->sender, {{"error", "speech replacement superseded"}});
            _replacement = event;
            _replacement->receiver = this;
            _cancelled = true;
            _player.Stop();
        } else Send(Failed, event.sender, {{"error", "speech service is busy"}});
        return true;
    }
    _busy = true; _cancelled = false; _speech = 0; _error.clear();
    _requester = event.sender;
    _prepare();
    _player.Speak({text, {}});
    return true;
}
