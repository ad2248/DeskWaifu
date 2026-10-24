#pragma once

#include "processor/Processor.hpp"
#include "screenshot/IScreenshotService.hpp"

class ScreenshotProcessor final : public Processor
{
public:
    explicit ScreenshotProcessor(ProcessorEventLoop& loop);
    ScreenshotProcessor(ProcessorEventLoop& loop, std::unique_ptr<IScreenshotService> service);
    bool HandleEvent(const ProcessorEvent& event) override;
private:
    std::unique_ptr<IScreenshotService> _service;
    IProcessor* _requester = nullptr;
    QString _capture;
};
