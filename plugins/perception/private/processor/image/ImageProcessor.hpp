#pragma once

#include "processor/Processor.hpp"
#include <QImage>
#include <functional>

class ImageProcessor final : public Processor
{
public:
    ImageProcessor(ProcessorEventLoop& loop, std::function<QImage()> clipboard, PluginContext* context = nullptr);
    ~ImageProcessor() override;
    void Stop() noexcept override;
    bool HandleEvent(const ProcessorEvent& event) override;
private:
    bool Read(const ProcessorEvent& event, RequestId request);
    void Complete(const QString& type, IProcessorPlugin* receiver, RequestId request, QVariantMap data = {});
    std::unique_ptr<EventEndpoint> _ownedEndpoint;
    EventEndpoint* _endpoint = nullptr;
    RequestId _request = 0;
    std::function<QImage()> _clipboard;
    IProcessorPlugin* _requester = nullptr;
    bool _busy = false, _cancelled = false, _stopped = false;
};
