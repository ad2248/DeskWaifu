#pragma once

#include "processor/Processor.hpp"
#include "SpeechPlayer.hpp"
#include <functional>
#include <optional>
#include <memory>

class SpeechProcessor final : public Processor
{
public:
    SpeechProcessor(ProcessorEventLoop& loop, SpeechPlayer& player);
    SpeechProcessor(ProcessorEventLoop& loop, const QString& apiKey);
    ~SpeechProcessor() override;
    void Stop();
    bool HandleEvent(const ProcessorEvent& event) override;
private:
    void ConnectPlayer();
    std::unique_ptr<MiniMaxSpeechSource> _source;
    std::unique_ptr<AudioPlayer> _audio;
    std::unique_ptr<SpeechPlayer> _ownedPlayer;
    SpeechPlayer& _player;
    IProcessor* _requester = nullptr;
    SpeechId _speech = 0;
    bool _busy = false, _cancelled = false;
    QString _error;
    std::optional<ProcessorEvent> _replacement;
    bool _stopped = false;
};
