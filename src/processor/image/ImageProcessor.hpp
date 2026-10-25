#pragma once

#include "processor/Processor.hpp"
#include <QImage>
#include <functional>

class ImageProcessor final : public Processor
{
public:
    ImageProcessor(ProcessorEventLoop& loop, std::function<QImage()> clipboard);
    bool HandleEvent(const ProcessorEvent& event) override;
private:
    std::function<QImage()> _clipboard;
    IProcessor* _requester = nullptr;
    bool _busy = false, _cancelled = false;
};
