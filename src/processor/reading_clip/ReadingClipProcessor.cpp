#include "ReadingClipProcessor.hpp"
#include "processor/Events.hpp"
#include <QJsonDocument>
#include <QJsonObject>

ReadingClipProcessor::ReadingClipProcessor(ProcessorEventLoop& loop, IProcessor& images, IProcessor& ocr, IProcessor& speech)
    : Processor(loop), _images(images), _ocr(ocr), _speech(speech) {}

void ReadingClipProcessor::SetState(State state)
{
    _state = state;
    Send(ProcessorEvents::ReadingState, nullptr, Status());
}

QVariantMap ReadingClipProcessor::Status() const
{
    const char* states[] = {"idle", "reading_image", "recognizing", "speaking", "cancelling"};
    return {{"ok", true}, {"state", states[int(_state)]}, {"error", _error}};
}

void ReadingClipProcessor::Finish(QString error)
{
    _waitingFor = nullptr;
    _image.clear();
    _error = std::move(error);
    SetState(State::Idle);
}

bool ReadingClipProcessor::HandleEvent(const ProcessorEvent& event)
{
    using namespace ProcessorEvents;
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
            _waitingFor = &_images;
            SetState(State::WaitingImage);
            Send(ReadImage, _waitingFor);
        } else if (_state != State::Cancelling) {
            SetState(State::Cancelling);
            Send(Cancel, _waitingFor);
        }
        return true;
    }
    if (event.receiver != this || !_waitingFor || event.sender != _waitingFor) return false;
    const bool terminal = event.type == Failed || event.type == ImageReady || event.type == Recognized || event.type == Spoken;
    if (!terminal) return false;
    if (_state == State::Cancelling) { Finish("reading cancelled"); return true; }
    if (event.type == Failed) { Finish(event.data.value("error").toString()); return true; }
    if (_state == State::WaitingImage && event.type == ImageReady) {
        _image = qvariant_cast<ImageFilePtr>(event.data.value("image"));
        if (!_image) { Finish("image service returned no temporary image"); return true; }
        _waitingFor = &_ocr;
        SetState(State::WaitingOcr);
        Send(Recognize, _waitingFor, {{"image", QVariant::fromValue(_image)}});
        return true;
    }
    if (_state == State::WaitingOcr && event.type == Recognized) {
        const auto text = event.data.value("text").toString();
        _image.clear();
        if (text.trimmed().isEmpty()) { Finish("no text recognized"); return true; }
        _waitingFor = &_speech;
        SetState(State::WaitingSpeech);
        Send(Speak, _waitingFor, {{"text", text}});
        return true;
    }
    if (_state == State::WaitingSpeech && event.type == Spoken) { Finish(); return true; }
    return false;
}
