#include "ScreenshotProcessor.hpp"
#include "processor/Events.hpp"

ScreenshotProcessor::ScreenshotProcessor(ProcessorEventLoop& loop)
    : ScreenshotProcessor(loop, CreateScreenshotService()) {}

ScreenshotProcessor::ScreenshotProcessor(ProcessorEventLoop& loop, std::unique_ptr<IScreenshotService> service)
    : Processor(loop), _service(std::move(service))
{
    connect(_service.get(), &IScreenshotService::Captured, this, [this](const QString& capture, const QImage& image) {
        if (capture != _capture) return;
        auto* receiver = _requester;
        _capture.clear(); _requester = nullptr;
        Send(ProcessorEvents::ScreenshotReady, receiver, {{"image", image}});
    });
    connect(_service.get(), &IScreenshotService::Failed, this, [this](const QString& capture, IScreenshotService::Error, const QString& error) {
        if (capture != _capture) return;
        auto* receiver = _requester;
        _capture.clear(); _requester = nullptr;
        Send(ProcessorEvents::Failed, receiver, {{"error", error}});
    });
}

bool ScreenshotProcessor::HandleEvent(const ProcessorEvent& event)
{
    if (event.type == ProcessorEvents::Cancel && !_capture.isEmpty() && event.sender == _requester) {
        _service->Cancel(_capture); return true;
    }
    if (event.type != ProcessorEvents::Screenshot) return false;
    if (_service->Busy()) Send(ProcessorEvents::Failed, event.sender, {{"error", "screenshot service is busy"}});
    else {
        _requester = event.sender;
        _capture = _service->Capture();
    }
    return true;
}
