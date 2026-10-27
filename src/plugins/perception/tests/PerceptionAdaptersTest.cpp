#include "processor/image/ImageProcessor.hpp"
#include "processor/image/ImageFile.hpp"
#include "processor/ocr/OcrProcessor.hpp"
#include "processor/screenshot/ScreenshotProcessor.hpp"
#include "processor/Events.hpp"
#include "EventLoopFence.hpp"
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
    Check(done(), "perception adapter timed out");
}
QImage SyntheticImage() { QImage image(7, 5, QImage::Format_ARGB32); image.fill(Qt::blue); return image; }
struct Sink : Processor
{
    using Processor::Processor;
    std::vector<ProcessorEvent> received;
    bool HandleEvent(const ProcessorEvent& event) override
    {
        if (event.receiver != this) return false;
        received.push_back(event); return true;
    }
};
class FakeScreenshot : public IScreenshotService
{
public:
    bool busy = false;
    QString Capture(const QString&) override
    {
        busy = true;
        QTimer::singleShot(0, this, [this] { if (busy) { busy = false; emit Captured("capture", SyntheticImage()); } });
        return "capture";
    }
    bool Cancel(const QString&) override
    { if (!busy) return false; busy = false; emit Failed("capture", Error::Cancelled, "cancelled"); return true; }
    bool Busy() const override { return busy; }
};
}
int main(int argc, char** argv)
try {
    QCoreApplication app(argc, argv);
    Check(argc == 2, "expected native fake OCR helper");
    using namespace ProcessorEvents;
    Check(!CreateImageFile(QImage()), "private image factory rejects null images");
    Check(!CreateImageFile(QImage(4097, 1, QImage::Format_ARGB32))
          && !CreateImageFile(QImage(1, 4097, QImage::Format_ARGB32)),
          "private image factory preserves image dimension limits");
    {
        ProcessorEventLoop loop;
        int reads = 0;
        ImageProcessor images(loop, [&] { Check(QThread::currentThread() == app.thread(), "clipboard access is on GUI thread"); ++reads; return SyntheticImage(); });
        Sink sink(loop);
        loop.Post({ReadImage, &sink, &images});
        Wait([&] { return sink.received.size() == 1; });
        Check(reads == 1 && sink.received[0].type == ImageReady && sink.received[0].sender == &images, "image service async completion");
        auto file = qvariant_cast<ImageFilePtr>(sink.received[0].data.value("image")); const auto path = file->Path();
        Check(QFileInfo::exists(path), "private PNG exists while referenced");
        Check(QImage(path) == SyntheticImage(), "private image factory preserves PNG contents");
        sink.received.clear(); file.clear(); Check(AwaitProcessorEvents(loop), "processor FIFO fence timed out");
        Check(!QFileInfo::exists(path), "private PNG is cleaned up by resource lifetime");
    }
    {
        ProcessorEventLoop loop;
        ImageProcessor images(loop, [] { return QImage(); }); Sink sink(loop);
        loop.Post({ReadImage, &sink, &images}); Wait([&] { return sink.received.size() == 1; });
        Check(sink.received[0].type == Failed, "empty clipboard failure without touching real clipboard");
    }
    {
        ProcessorEventLoop loop;
        ScreenshotProcessor screenshots(loop, std::make_unique<FakeScreenshot>()); Sink sink(loop);
        loop.Post({Screenshot, &sink, &screenshots}); Wait([&] { return sink.received.size() == 1; });
        Check(sink.received[0].type == ScreenshotReady && qvariant_cast<QImage>(sink.received[0].data.value("image")).size() == QSize(7, 5), "screenshot service event adapter");
    }
    for (const auto* mode : {"", "load-error", "hang"}) {
        qputenv("DESKWAIFU_OCR_TEST_MODE", mode);
        ProcessorEventLoop loop;
        auto client = std::make_unique<OcrClient>(app.arguments()[1], 1000, 50);
        auto* observedClient = client.get();
        OcrProcessor ocr(loop, std::move(client)); Sink sink(loop);
        auto image = CreateImageFile(SyntheticImage()); const auto path = image->Path();
        loop.Post({Recognize, &sink, &ocr, {{"image", QVariant::fromValue(image)}}}); image.clear();
        if (QString::fromLatin1(mode) == "hang") {
            Wait([&] { return observedClient->Status().value("state") == "pending"; });
            loop.Post({Cancel, &sink, &ocr});
        }
        Wait([&] { return sink.received.size() == 1; });
        Check(sink.received[0].sender == &ocr && sink.received[0].type == (QString::fromLatin1(mode).isEmpty() ? Recognized : Failed), "OCR proxy converts wire completion to pointer-routed event");
        Wait([&] { return !observedClient->Status().value("running").toBool(); });
        Check(!QFileInfo::exists(path), "OCR temporary file released after worker use");
    }
    qunsetenv("DESKWAIFU_OCR_TEST_MODE");
    std::cout << "Image, screenshot and OCR capability adapter tests passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
