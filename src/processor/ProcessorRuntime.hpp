#pragma once

#include "event/ProcessorEventLoop.hpp"
#include "image/ImageProcessor.hpp"
#include "ocr/OcrProcessor.hpp"
#include "screenshot/ScreenshotProcessor.hpp"

// 过渡期尚未迁移的内置处理器；不组装插件内部的业务流程。
class ProcessorRuntime
{
public:
    explicit ProcessorRuntime(std::function<QImage()> clipboard)
        : _images(_loop, std::move(clipboard)), _ocr(_loop), _screenshots(_loop) {}
    ~ProcessorRuntime()
    {
        // 所有请求者仍注册时，先收尾服务/工作线程；成员析构随后才注销。
        _images.Stop(); _ocr.Stop(); _screenshots.Stop();
    }
    ProcessorEventLoop& Loop() { return _loop; }
private:
    ProcessorEventLoop _loop; // 比所有处理器和排队指针存活更久。
    ImageProcessor _images;
    OcrProcessor _ocr;
    ScreenshotProcessor _screenshots;
};
