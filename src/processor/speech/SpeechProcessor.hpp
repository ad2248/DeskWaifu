#pragma once

#include "processor/Processor.hpp"
#include "SpeechPlayer.hpp"
#include <functional>
#include <optional>

class SpeechProcessor final : public Processor
{
public:
    SpeechProcessor(ProcessorEventLoop& loop, SpeechPlayer& player, std::function<void()> prepare);
    ~SpeechProcessor() override;
    bool HandleEvent(const ProcessorEvent& event) override;
private:
    SpeechPlayer& _player;
    std::function<void()> _prepare;
    IProcessor* _requester = nullptr;
    SpeechId _speech = 0;
    bool _busy = false, _cancelled = false;
    QString _error;
    std::optional<ProcessorEvent> _replacement;
};
