#pragma once

#include "processor/Processor.hpp"
#include "IScreenshotService.hpp"

class ScreenshotProcessor final : public Processor
{
public:
    ~ScreenshotProcessor() override;
    void Stop() noexcept override;
    explicit ScreenshotProcessor(ProcessorEventLoop& loop);
    ScreenshotProcessor(ProcessorEventLoop& loop, std::unique_ptr<IScreenshotService> service);
    bool HandleEvent(const ProcessorEvent& event) override;
private:
    std::unique_ptr<IScreenshotService> _service;
    IProcessorPlugin* _requester = nullptr;
    QString _capture;
};
