#pragma once

#include "processor/Processor.hpp"
#include "contracts/ImageFile.hpp"

class ReadingClipProcessor final : public Processor
{
public:
    enum class State { Idle, WaitingImage, WaitingOcr, WaitingSpeech, Cancelling };
    ReadingClipProcessor(ProcessorEventLoop& loop, IProcessorPlugin& images, IProcessorPlugin& ocr, IProcessorPlugin& speech);
    ReadingClipProcessor(ProcessorEventLoop& loop, EventEndpoint& endpoint);
    ~ReadingClipProcessor() override;
    void Stop() noexcept override;
    void HandleDelivery(const EventDelivery& delivery);
    bool HandleEvent(const ProcessorEvent& event) override;
    State CurrentState() const { return _state; }
    QString LastError() const { return _error; }
    QVariantMap Status() const;
private:
    void SetState(State state);
    void Finish(QString error = {});
    void Step(State state, const QString& type, const QString& capability, IProcessorPlugin* legacy, QVariantMap data = {});
    bool Result(const QString& type, const QVariantMap& data);
    IProcessorPlugin* _images = nullptr;
    IProcessorPlugin* _ocr = nullptr;
    IProcessorPlugin* _speech = nullptr;
    std::unique_ptr<EventEndpoint> _ownedEndpoint;
    EventEndpoint* _endpoint = nullptr;
    RequestId _request = 0;
    IProcessorPlugin* _waitingFor = nullptr;
    State _state = State::Idle;
    ImageFilePtr _image;
    QString _error;
    bool _stopped = false;
};
