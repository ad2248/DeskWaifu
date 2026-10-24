#pragma once

#include "event/ProcessorEventLoop.hpp"
#include "Events.hpp"
#include "image/ImageProcessor.hpp"
#include "ocr/OcrProcessor.hpp"
#include "screenshot/ScreenshotProcessor.hpp"
#include "speech/SpeechProcessor.hpp"
#include "reading_clip/ReadingClipProcessor.hpp"

// Composition only. Business sequencing lives in ReadingClipProcessor, not here.
class ProcessorRuntime
{
public:
    ProcessorRuntime(SpeechPlayer& speech, std::function<void()> prepareSpeech, std::function<QImage()> clipboard)
        : _images(_loop, std::move(clipboard)), _ocr(_loop), _screenshots(_loop),
          _speech(_loop, speech, std::move(prepareSpeech)), _reading(_loop, _images, _ocr, _speech) {}
    ProcessorEventLoop& Loop() { return _loop; }
    OcrClient& Ocr() { return _ocr.Client(); }
    ReadingClipProcessor& Reading() { return _reading; }
    bool ReadClipboard() { return _loop.Post({ProcessorEvents::ReadingClip}); }
    void SpeakGreeting(const QString& text)
    { _loop.Post({ProcessorEvents::Speak, nullptr, &_speech, {{"text", text}, {"replace", true}}}); }
private:
    ProcessorEventLoop _loop; // Outlives every handler and queued pointer.
    ImageProcessor _images;
    OcrProcessor _ocr;
    ScreenshotProcessor _screenshots;
    SpeechProcessor _speech;
    ReadingClipProcessor _reading;
};
