#pragma once

#include "event/ProcessorEventLoop.hpp"
#include "Events.hpp"
#include "image/ImageProcessor.hpp"
#include "ocr/OcrProcessor.hpp"
#include "screenshot/ScreenshotProcessor.hpp"
#include "speech/SpeechProcessor.hpp"
#include "reading_clip/ReadingClipProcessor.hpp"

// 仅负责组装，业务流程顺序由 ReadingClipProcessor 管理，不在此处实现。
class ProcessorRuntime
{
public:
    ProcessorRuntime(const QString& apiKey, std::function<QImage()> clipboard)
        : _images(_loop, std::move(clipboard)), _ocr(_loop), _screenshots(_loop),
          _speech(_loop, apiKey), _reading(_loop, _images, _ocr, _speech) {}
    ~ProcessorRuntime()
    {
        // 所有请求者仍注册时，先收尾服务/工作线程；成员析构随后才注销。
        _images.Stop(); _ocr.Stop(); _screenshots.Stop(); _speech.Stop();
    }
    ProcessorEventLoop& Loop() { return _loop; }
private:
    ProcessorEventLoop _loop; // 比所有处理器和排队指针存活更久。
    ImageProcessor _images;
    OcrProcessor _ocr;
    ScreenshotProcessor _screenshots;
    SpeechProcessor _speech;
    ReadingClipProcessor _reading;
};
