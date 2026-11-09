#pragma once

#include "SpeechProcessor.hpp"

// Plugin-private bridge onto the existing speech state machine. It registers no
// capabilities itself: exactly one plugin endpoint provides speech.speak/v1.
class SpeechEndpoint final : public Processor
{
public:
    SpeechEndpoint(ProcessorEventLoop& loop, EventEndpoint& endpoint, const QString& apiKey);
    SpeechEndpoint(ProcessorEventLoop& loop, EventEndpoint& endpoint, SpeechPlayer& player);
    ~SpeechEndpoint() override;
    void Speak(RequestId request, const QVariantMap& data);
    void Cancel(RequestId request);
    void SetApiKey(const QString& key);
    void Stop() noexcept override;
    bool HandleEvent(const ProcessorEvent& event) override;

private:
    EventEndpoint& _endpoint;
    SpeechProcessor _speech;
};
