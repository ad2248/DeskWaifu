#include "SpeechEndpoint.hpp"
#include "contracts/SpeechEvents.hpp"

SpeechEndpoint::SpeechEndpoint(ProcessorEventLoop& loop, EventEndpoint& endpoint, const QString& apiKey)
    : Processor(loop), _endpoint(endpoint), _speech(loop, apiKey) {}
SpeechEndpoint::SpeechEndpoint(ProcessorEventLoop& loop, EventEndpoint& endpoint, SpeechPlayer& player)
    : Processor(loop), _endpoint(endpoint), _speech(loop, player) {}
SpeechEndpoint::~SpeechEndpoint() { Stop(); }

void SpeechEndpoint::Speak(RequestId request, const QVariantMap& data)
{
    auto correlated = data;
    correlated.insert("request_id", QVariant::fromValue(request));
    // Already on the bus thread. Do not enqueue again: a cancellation must not
    // overtake acceptance, and an accepted replacement retains its FIFO order.
    _speech.HandleEvent({SpeechEvents::Speak, this, &_speech, std::move(correlated)});
}

void SpeechEndpoint::Cancel(RequestId request)
{
    if (request) _speech.HandleEvent({"operation.cancel", this, &_speech, {{"request_id", QVariant::fromValue(request)}}});
}

void SpeechEndpoint::SetApiKey(const QString& key)
{ _speech.HandleEvent({"speech.configure", this, &_speech, {{"api_key", key}}}); }
void SpeechEndpoint::Stop() noexcept { _speech.Stop(); Processor::Stop(); }

bool SpeechEndpoint::HandleEvent(const ProcessorEvent& event)
{
    if (event.receiver != this || event.sender != &_speech ||
        (event.type != SpeechEvents::Completed && event.type != "operation.failed")) return false;
    const auto request = event.data.value("request_id").toULongLong();
    // Zero belongs to plugin-local notifications (the double-click greeting).
    if (request) {
        QVariantMap data;
        if (event.type == "operation.failed") data.insert("error", event.data.value("error"));
        _endpoint.Respond(request, std::make_shared<ProcessorEvent>(event.type, nullptr, nullptr, std::move(data)));
    }
    return true;
}
