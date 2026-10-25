#include "ReadingClipProcessor.hpp"
#include "processor/Events.hpp"
#include <QJsonDocument>
#include <QJsonObject>

ReadingClipProcessor::ReadingClipProcessor(ProcessorEventLoop& loop, IProcessor& images, IProcessor& ocr, IProcessor& speech)
    : Processor(loop), _images(&images), _ocr(&ocr), _speech(&speech)
{
    // Kept for existing injected processor contracts during the migration.
    _ownedEndpoint = loop.CreateEndpoint(QStringLiteral("reading-fixture-%1").arg(quintptr(this)),
        [this](const EventDelivery& delivery) { HandleDelivery(delivery); });
    _endpoint = _ownedEndpoint.get();
    if (_endpoint) {
        _endpoint->Provide("command.reading_clip/v1");
        _endpoint->Provide("command.get_reading_clip_status/v1");
    }
}

ReadingClipProcessor::ReadingClipProcessor(ProcessorEventLoop& loop, EventEndpoint& endpoint)
    : Processor(loop), _endpoint(&endpoint) {}

ReadingClipProcessor::~ReadingClipProcessor() { Stop(); }

void ReadingClipProcessor::Stop()
{
    if (_stopped) return;
    _stopped = true;
    if (_request) _endpoint->Cancel(_request);
    else if (_waitingFor) Send(ProcessorEvents::Cancel, _waitingFor);
    _request = 0; _waitingFor = nullptr; _image.clear();
    if (_ownedEndpoint) _ownedEndpoint->Invalidate();
}

void ReadingClipProcessor::HandleDelivery(const EventDelivery& delivery)
{
    if (_stopped || delivery.kind != EventDelivery::Kind::Request) return;
    QVariantMap response;
    if (delivery.topic == "command.reading_clip/v1") {
        Send(ProcessorEvents::ReadingClip, this);
        response = {{"ok", true}, {"queued", true}};
    } else response = Status();
    _endpoint->Respond(delivery.requestId, std::make_shared<ProcessorEvent>("command.result", nullptr, nullptr,
        QVariantMap{{"json", QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(response)).toJson(QJsonDocument::Compact))}}));
}

void ReadingClipProcessor::SetState(State state)
{
    _state = state;
    Send(ProcessorEvents::ReadingState, nullptr, Status());
    if (_endpoint) _endpoint->Notify("reading.state/v1", std::make_shared<ProcessorEvent>(ProcessorEvents::ReadingState, nullptr, nullptr, Status()));
}

QVariantMap ReadingClipProcessor::Status() const
{
    const char* states[] = {"idle", "reading_image", "recognizing", "speaking", "cancelling"};
    return {{"ok", true}, {"state", states[int(_state)]}, {"error", _error}};
}

void ReadingClipProcessor::Finish(QString error)
{
    _waitingFor = nullptr;
    _request = 0;
    _image.clear();
    _error = std::move(error);
    SetState(State::Idle);
}

bool ReadingClipProcessor::HandleEvent(const ProcessorEvent& event)
{
    using namespace ProcessorEvents;
    if (_stopped) return false;
    if (event.type == "reading.command") {
        const auto command = QJsonDocument::fromJson(event.data.value("json").toString().toUtf8()).object().value("command").toString();
        if (command == "reading_clip") {
            Send(ReadingClip, this);
            Send("reading.result", event.sender, {{"json", QStringLiteral("{\"ok\":true,\"queued\":true}")}});
        } else Send("reading.result", event.sender, {{"json", QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(Status())).toJson(QJsonDocument::Compact))}});
        return true;
    }
    if (event.type == ReadingClip) {
        if (_state == State::Idle) {
            _error.clear();
            Step(State::WaitingImage, ReadImage, "image.read/v1", _images);
        } else if (_state != State::Cancelling) {
            SetState(State::Cancelling);
            if (_request) _endpoint->Cancel(_request);
            else Send(Cancel, _waitingFor);
        }
        return true;
    }
    if (event.receiver != this || !_waitingFor || event.sender != _waitingFor) return false;
    return Result(event.type, event.data);
}

void ReadingClipProcessor::Step(State state, const QString& type, const QString& capability, IProcessor* legacy, QVariantMap data)
{
    SetState(state);
    _waitingFor = legacy;
    if (legacy) { Send(type, legacy, std::move(data)); return; }
    _request = _endpoint->Request(capability, std::make_shared<ProcessorEvent>(type, nullptr, nullptr, std::move(data)),
        [this](const RequestResult& result) {
            if (_stopped || result.requestId != _request) return;
            _request = 0;
            if (_state == State::Cancelling) { Finish("reading cancelled"); return; }
            if (result.status != RequestStatus::Replied || !result.payload) {
                Finish("reading capability: " + QString::fromLatin1(RequestStatusName(result.status))); return;
            }
            if (!Result(result.payload->type, result.payload->data)) Finish("invalid reading capability response");
        }, 65000);
    if (!_request) Finish("reading endpoint unavailable");
}

bool ReadingClipProcessor::Result(const QString& type, const QVariantMap& data)
{
    using namespace ProcessorEvents;
    const bool terminal = type == Failed || type == ImageReady || type == Recognized || type == Spoken;
    if (!terminal) return false;
    if (_state == State::Cancelling) { Finish("reading cancelled"); return true; }
    if (type == Failed) { Finish(data.value("error").toString()); return true; }
    if (_state == State::WaitingImage && type == ImageReady) {
        _image = qvariant_cast<ImageFilePtr>(data.value("image"));
        if (!_image) { Finish("image service returned no temporary image"); return true; }
        Step(State::WaitingOcr, Recognize, "ocr.recognize/v1", _ocr, {{"image", QVariant::fromValue(_image)}});
        return true;
    }
    if (_state == State::WaitingOcr && type == Recognized) {
        const auto text = data.value("text").toString();
        _image.clear();
        if (text.trimmed().isEmpty()) { Finish("no text recognized"); return true; }
        Step(State::WaitingSpeech, Speak, "speech.speak/v1", _speech, {{"text", text}});
        return true;
    }
    if (_state == State::WaitingSpeech && type == Spoken) { Finish(); return true; }
    return false;
}
