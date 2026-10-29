#include "processor/reading_clip/ReadingClipProcessor.hpp"
#include "processor/Events.hpp"
#include "EventLoopFence.hpp"
#include "TestImageFile.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QThread>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Wait(const std::function<bool()>& done)
{
    QElapsedTimer timer; timer.start();
    while (!done() && timer.elapsed() < 3000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    Check(done(), "reading pipeline timed out");
}
void Drain(ProcessorEventLoop& loop) { Check(AwaitProcessorEvents(loop), "processor FIFO fence timed out"); }
QImage SyntheticImage() { QImage image(7, 5, QImage::Format_ARGB32); image.fill(Qt::blue); return image; }
struct Capability : Processor
{
    QString type;
    int requests = 0, cancels = 0;
    ProcessorEvent last;
    Capability(ProcessorEventLoop& loop, QString accepted) : Processor(loop), type(std::move(accepted)) {}
    bool HandleEvent(const ProcessorEvent& event) override
    {
        if (event.type == ProcessorEvents::Cancel) { ++cancels; return true; }
        if (event.type != type) return false;
        ++requests; last = event; return true;
    }
};
}
int main(int argc, char** argv)
try {
    QCoreApplication app(argc, argv);
    using namespace ProcessorEvents;
    {
        ProcessorEventLoop loop;
        Capability images(loop, ReadImage), ocr(loop, Recognize), speech(loop, Speak);
        ReadingClipProcessor reading(loop, images, ocr, speech);
        loop.Post({ReadingClip, nullptr, &reading});
        Wait([&] { return images.requests == 1; });
        Check(reading.CurrentState() == ReadingClipProcessor::State::WaitingImage && images.last.sender == &reading, "image request sender pointer");
        auto image = MakeTestImageFile(SyntheticImage()); const auto path = image->Path();
        loop.Post({ImageReady, &images, &reading, {{"image", QVariant::fromValue(image)}}}); image.clear();
        Wait([&] { return ocr.requests == 1; });
        Check(reading.CurrentState() == ReadingClipProcessor::State::WaitingOcr && QFileInfo::exists(path), "image remains alive through queued OCR request");
        ocr.last = {};
        loop.Post({Recognized, &ocr, &reading, {{"text", "姐姐，你好"}}});
        Wait([&] { return speech.requests == 1; });
        Check(!QFileInfo::exists(path), "temporary PNG is released after recognition");
        Check(speech.last.sender == &reading && speech.last.data.value("text") == "姐姐，你好", "speech request uses pointer reply route");
        loop.Post({Failed, &speech, nullptr, {{"error", "unrelated originless speech notification"}}}); Drain(loop);
        Check(reading.CurrentState() == ReadingClipProcessor::State::WaitingSpeech, "broadcast notifications must not complete a directed operation");
        loop.Post({Spoken, &speech, &reading});
        Wait([&] { return reading.CurrentState() == ReadingClipProcessor::State::Idle; });
        Check(reading.LastError().isEmpty(), "successful pipeline");
    }
    {
        ProcessorEventLoop loop;
        Capability images(loop, ReadImage), ocr(loop, Recognize), speech(loop, Speak);
        ReadingClipProcessor reading(loop, images, ocr, speech);
        loop.Post({ReadingClip, nullptr, &reading}); Wait([&] { return images.requests == 1; });
        loop.Post({Failed, &ocr, &reading, {{"error", "unrelated"}}}); Drain(loop);
        Check(reading.CurrentState() == ReadingClipProcessor::State::WaitingImage, "ignore results from a different capability");
        loop.Post({ReadingClip, nullptr, &reading}); Wait([&] { return images.cancels == 1; });
        loop.Post({ReadingClip, nullptr, &reading}); Drain(loop);
        Check(reading.CurrentState() == ReadingClipProcessor::State::Cancelling && images.requests == 1 && images.cancels == 1, "wait for cancellation drain without starting another flow");
        loop.Post({ImageReady, &images, &reading, {{"image", QVariant::fromValue(MakeTestImageFile(SyntheticImage()))}}});
        Wait([&] { return reading.CurrentState() == ReadingClipProcessor::State::Idle; });
        Check(ocr.requests == 0 && speech.requests == 0, "discard terminal image result during cancellation");
        loop.Post({ReadingClip, nullptr, &reading}); Wait([&] { return images.requests == 2; });
        loop.Post({Failed, &images, &reading, {{"error", "empty clipboard"}}});
        Wait([&] { return reading.CurrentState() == ReadingClipProcessor::State::Idle; });
        Check(reading.LastError() == "empty clipboard", "failure returns pipeline to idle");
    }
    for (bool duringSpeech : {false, true}) {
        ProcessorEventLoop loop;
        Capability images(loop, ReadImage), ocr(loop, Recognize), speech(loop, Speak);
        ReadingClipProcessor reading(loop, images, ocr, speech);
        loop.Post({ReadingClip}); Wait([&] { return images.requests == 1; });
        loop.Post({ImageReady, &images, &reading, {{"image", QVariant::fromValue(MakeTestImageFile(SyntheticImage()))}}});
        Wait([&] { return ocr.requests == 1; });
        if (duringSpeech) {
            ocr.last = {};
            loop.Post({Recognized, &ocr, &reading, {{"text", "test"}}});
            Wait([&] { return speech.requests == 1; });
        }
        loop.Post({ReadingClip});
        Wait([&] { return duringSpeech ? speech.cancels == 1 : ocr.cancels == 1; });
        Check(reading.CurrentState() == ReadingClipProcessor::State::Cancelling, "OCR/speech cancellation waits for the service terminal event");
        loop.Post({duringSpeech ? Spoken : Recognized, duringSpeech ? static_cast<IProcessorPlugin*>(&speech) : &ocr,
            &reading, {{"text", "late result"}}});
        Wait([&] { return reading.CurrentState() == ReadingClipProcessor::State::Idle; });
        Check(reading.LastError() == "reading cancelled" && speech.requests == (duringSpeech ? 1 : 0), "late completion cannot resume cancelled reading");
        loop.Post({ReadingClip}); Wait([&] { return images.requests == 2; });
        loop.Post({Failed, &images, &reading, {{"error", "finish retry"}}});
        Wait([&] { return reading.CurrentState() == ReadingClipProcessor::State::Idle; });
    }
    std::cout << "Stateful clipboard reading and cancellation tests passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
