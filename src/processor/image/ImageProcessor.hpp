#pragma once

#include "processor/Processor.hpp"
#include <QImage>
#include <functional>

class ImageProcessor final : public Processor
{
public:
    ImageProcessor(ProcessorEventLoop& loop, std::function<QImage()> clipboard);
    ~ImageProcessor() override;
    void Stop();
    bool HandleEvent(const ProcessorEvent& event) override;
private:
    bool Read(const ProcessorEvent& event, RequestId request);
    void Complete(const QString& type, IProcessor* receiver, RequestId request, QVariantMap data = {});
    std::unique_ptr<EventEndpoint> _endpoint;
    RequestId _request = 0;
    std::function<QImage()> _clipboard;
    IProcessor* _requester = nullptr;
    bool _busy = false, _cancelled = false, _stopped = false;
};
