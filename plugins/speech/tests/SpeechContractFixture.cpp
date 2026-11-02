#include "plugin/PluginApi.hpp"
#include "plugin/PluginContext.hpp"
#include "processor/speech/SpeechEndpoint.hpp"
#include "processor/speech/source/MiniMaxSpeechSource.hpp"
#include "processor/speech/audio/AudioPlayer.hpp"
#include <QTimer>

namespace {
struct Source : MiniMaxSpeechSource
{
    Source() : MiniMaxSpeechSource({}) {}
    QList<SpeechId> starts, cancels;
    void Start(SpeechId id, SpeechRequest) override { starts.append(id); }
    void Cancel(SpeechId id) override { cancels.append(id); }
};
struct Audio : AudioPlayer
{
    QList<SpeechId> starts, stops, drains;
    QByteArray pcm;
    void Start(SpeechId id, QAudioFormat) override { starts.append(id); }
    void Append(SpeechId, QByteArray data) override { pcm += data; }
    void Stop(SpeechId id) override { stops.append(id); pcm.clear(); }
    void FinishInput(SpeechId id) override
    { drains.append(id); QTimer::singleShot(0, this, [this, id] { emit Drained(id); }); }
};
class SpeechContractFixture final : public IProcessorPlugin
{
public:
    bool Initialize(PluginContext& context, QString*) override
    {
        _speech = context.CreateEndpoint("speech", [this](const EventDelivery& delivery) {
            if (delivery.kind == EventDelivery::Kind::Cancellation) _bridge->Cancel(delivery.requestId);
            else if (delivery.kind == EventDelivery::Kind::Request && delivery.payload) _bridge->Speak(delivery.requestId, delivery.payload->data);
        });
        if (!_speech || !_speech->Provide("speech.speak/v1")) return false;
        _bridge.reset(new SpeechEndpoint(context.Loop(), *_speech, _player));
        _control = context.CreateEndpoint("control", [this](const EventDelivery& delivery) {
            if (delivery.kind != EventDelivery::Kind::Request || !delivery.payload) return;
            const auto action = delivery.payload->data.value("action").toString();
            const auto id = _source.starts.isEmpty() ? SpeechId{} : _source.starts.last();
            if (action == "finish" || action == "late") {
                emit _source.Ready(id, {});
                emit _source.AudioReceived(id, action == "late" ? "late" : "pcm");
                emit _source.Completed(id);
                if (action == "late") emit _audio.Drained(id);
            }
            _control->Respond(delivery.requestId, std::make_shared<ProcessorEvent>("test.speech.state", nullptr, nullptr,
                QVariantMap{{"starts", _source.starts.size()}, {"cancels", _source.cancels.size()},
                    {"stops", _audio.stops.size()}, {"pcm_empty", _audio.pcm.isEmpty()},
                    {"cancel_id", _source.cancels.isEmpty() ? 0 : _source.cancels.last()},
                    {"stop_id", _audio.stops.isEmpty() ? 0 : _audio.stops.last()}, {"id", id}}));
        });
        return _control && _control->Provide("test.speech.control/v1");
    }
    void Stop() noexcept override { _bridge.reset(); _speech = nullptr; _control = nullptr; }
private:
    Source _source;
    Audio _audio;
    SpeechPlayer _player{_source, _audio};
    EventEndpoint* _speech = nullptr;
    EventEndpoint* _control = nullptr;
    std::unique_ptr<SpeechEndpoint> _bridge;
};
}
DESKWAIFU_EXPORT_PLUGIN(SpeechContractFixture)
