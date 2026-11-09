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
    void Stop() noexcept override;
    bool HandleEvent(const ProcessorEvent& event) override;
private:
    void ConnectPlayer();
    void Reply(IProcessorPlugin* receiver, quint64 requestId, bool completed, const QString& error = {});
    std::unique_ptr<MiniMaxSpeechSource> _source;
    std::unique_ptr<AudioPlayer> _audio;
    std::unique_ptr<SpeechPlayer> _ownedPlayer;
    SpeechPlayer& _player;
    IProcessorPlugin* _requester = nullptr;
    quint64 _requestId = 0; // Optional correlation for plugin adapters; legacy callers omit it.
    SpeechId _speech = 0;
    bool _busy = false, _cancelled = false;
    QString _error;
    std::optional<ProcessorEvent> _replacement;
    bool _stopped = false;
};
