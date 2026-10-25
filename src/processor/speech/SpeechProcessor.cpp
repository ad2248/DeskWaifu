#include "SpeechProcessor.hpp"
#include "processor/Events.hpp"
#include "source/MiniMaxSpeechSource.hpp"
#include "audio/AudioPlayer.hpp"
#include <iostream>

SpeechProcessor::SpeechProcessor(ProcessorEventLoop& loop, SpeechPlayer& player)
    : Processor(loop), _player(player)
{ ConnectPlayer(); }

SpeechProcessor::SpeechProcessor(ProcessorEventLoop& loop, const QString& apiKey)
    : Processor(loop), _source(new MiniMaxSpeechSource(apiKey)), _audio(new AudioPlayer),
      _ownedPlayer(new SpeechPlayer(*_source, *_audio)), _player(*_ownedPlayer)
{ ConnectPlayer(); }

void SpeechProcessor::ConnectPlayer()
{
    connect(&_player, &SpeechPlayer::Accepted, this, [this](SpeechId speech) {
        if (_busy && !_speech) _speech = speech;
    });
    connect(&_player, &SpeechPlayer::ErrorOccurred, this, [this](SpeechId speech, const SpeechError& error) {
        std::cout << "[Speech] " << error.message.toStdString() << '\n';
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
            // 已接收的替换必须在后续排队请求之前接管状态；重新入队会让
            // 更新的替换先启动，再被这个旧请求反向覆盖。
            HandleEvent(next);
        }
    });
}

SpeechProcessor::~SpeechProcessor()
{ Stop(); }

void SpeechProcessor::Stop()
{
    if (_stopped) return;
    _stopped = true;
    _player.disconnect(this);
    if (_busy) _player.Stop();
    _busy = false; _replacement.reset(); _requester = nullptr;
    _ownedPlayer.reset(); _audio.reset(); _source.reset();
}

bool SpeechProcessor::HandleEvent(const ProcessorEvent& event)
{
    using namespace ProcessorEvents;
    if (_stopped) {
        if (event.type == Speak) Send(Failed, event.sender, {{"error", "speech service stopped"}});
        return event.type == Speak || event.type == "speech.configure";
    }
    if (event.type == "speech.configure") { _player.SetApiKey(event.data.value("api_key").toString()); return true; }
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
    _player.Speak({text, {}});
    return true;
}
