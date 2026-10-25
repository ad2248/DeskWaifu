#pragma once

#include "processor/Processor.hpp"
#include "processor/image/ImageFile.hpp"

class ReadingClipProcessor final : public Processor
{
public:
    enum class State { Idle, WaitingImage, WaitingOcr, WaitingSpeech, Cancelling };
    ReadingClipProcessor(ProcessorEventLoop& loop, IProcessor& images, IProcessor& ocr, IProcessor& speech);
    bool HandleEvent(const ProcessorEvent& event) override;
    State CurrentState() const { return _state; }
    QString LastError() const { return _error; }
    QVariantMap Status() const;
private:
    void SetState(State state);
    void Finish(QString error = {});
    IProcessor& _images;
    IProcessor& _ocr;
    IProcessor& _speech;
    IProcessor* _waitingFor = nullptr;
    State _state = State::Idle;
    ImageFilePtr _image;
    QString _error;
};
