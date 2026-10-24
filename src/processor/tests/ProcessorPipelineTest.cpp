#include "processor/Events.hpp"
#include "processor/image/ImageProcessor.hpp"
#include "processor/image/ImageFile.hpp"
#include "processor/ocr/OcrProcessor.hpp"
#include "processor/reading_clip/ReadingClipProcessor.hpp"
#include "processor/screenshot/ScreenshotProcessor.hpp"
#include "processor/speech/SpeechProcessor.hpp"
#include "processor/ipc/IpcProcessor.hpp"
#include "MiniMaxSpeechSource.hpp"
#include "AudioPlayer.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QThread>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Wait(const std::function<bool()>& predicate)
{
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 3000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    Check(predicate(), "event-loop wait timed out");
}
void Drain() { for (int i = 0; i < 30; ++i) QCoreApplication::processEvents(); }
QImage SyntheticImage()
{
    QImage image(7, 5, QImage::Format_ARGB32); image.fill(Qt::blue); return image;
}
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
struct Commands : ICommandService
{
    int count = 0, disconnected = 0;
    bool large = false;
    std::string Execute(const std::string&, const std::string&) override
    { ++count; return large ? std::string(128 * 1024, 'x') : "{\"ok\":true}"; }
    void Disconnected(const std::string&) override { ++disconnected; }
};
}

int main(int argc, char** argv)
try {
    QCoreApplication app(argc, argv);
    Check(argc == 2, "expected native fake OCR helper");
    using namespace ProcessorEvents;
    {
        ProcessorEventLoop loop;
        Capability images(loop, ReadImage), ocr(loop, Recognize), speech(loop, Speak);
        ReadingClipProcessor reading(loop, images, ocr, speech);
        loop.Post({ReadingClip, nullptr, &reading});
        Wait([&] { return images.requests == 1; });
        Check(reading.CurrentState() == ReadingClipProcessor::State::WaitingImage && images.last.sender == &reading, "image request sender pointer");
        auto image = ImageFile::Create(SyntheticImage());
        const auto path = image->Path();
        loop.Post({ImageReady, &images, &reading, {{"image", QVariant::fromValue(image)}}});
        image.clear();
        Wait([&] { return ocr.requests == 1; });
        Check(reading.CurrentState() == ReadingClipProcessor::State::WaitingOcr && QFileInfo::exists(path), "image remains alive through queued OCR request");
        ocr.last = {}; // The fake service has finished its use of the image.
        loop.Post({Recognized, &ocr, &reading, {{"text", "姐姐，你好"}}});
        Wait([&] { return speech.requests == 1; });
        Check(!QFileInfo::exists(path), "temporary PNG is released after recognition");
        Check(speech.last.sender == &reading && speech.last.data.value("text") == "姐姐，你好", "speech request uses pointer reply route");
        loop.Post({Failed, &speech, nullptr, {{"error", "unrelated originless speech notification"}}}); Drain();
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
        loop.Post({Failed, &ocr, &reading, {{"error", "unrelated"}}}); Drain();
        Check(reading.CurrentState() == ReadingClipProcessor::State::WaitingImage, "ignore results from a different capability");
        loop.Post({ReadingClip, nullptr, &reading}); Wait([&] { return images.cancels == 1; });
        loop.Post({ReadingClip, nullptr, &reading}); Drain();
        Check(reading.CurrentState() == ReadingClipProcessor::State::Cancelling && images.requests == 1 && images.cancels == 1, "wait for cancellation drain without starting another flow");
        loop.Post({ImageReady, &images, &reading, {{"image", QVariant::fromValue(ImageFile::Create(SyntheticImage()))}}});
        Wait([&] { return reading.CurrentState() == ReadingClipProcessor::State::Idle; });
        Check(ocr.requests == 0 && speech.requests == 0, "discard terminal image result during cancellation");
        loop.Post({ReadingClip, nullptr, &reading}); Wait([&] { return images.requests == 2; });
        loop.Post({Failed, &images, &reading, {{"error", "empty clipboard"}}});
        Wait([&] { return reading.CurrentState() == ReadingClipProcessor::State::Idle; });
        Check(reading.LastError() == "empty clipboard", "failure returns pipeline to idle");
    }
    {
        ProcessorEventLoop loop;
        int reads = 0;
        ImageProcessor images(loop, [&] { Check(QThread::currentThread() == app.thread(), "clipboard access is on GUI thread"); ++reads; return SyntheticImage(); });
        Sink sink(loop);
        loop.Post({ReadImage, &sink, &images});
        Wait([&] { return sink.received.size() == 1; });
        Check(reads == 1 && sink.received[0].type == ImageReady && sink.received[0].sender == &images, "image service async completion");
        auto file = qvariant_cast<ImageFilePtr>(sink.received[0].data.value("image"));
        const auto path = file->Path();
        Check(QFileInfo::exists(path), "private PNG exists while referenced");
        sink.received.clear(); file.clear(); Drain();
        Check(!QFileInfo::exists(path), "private PNG is cleaned up by resource lifetime");
    }
    for (bool duringSpeech : {false, true}) {
        ProcessorEventLoop loop;
        Capability images(loop, ReadImage), ocr(loop, Recognize), speech(loop, Speak);
        ReadingClipProcessor reading(loop, images, ocr, speech);
        loop.Post({ReadingClip}); Wait([&] { return images.requests == 1; });
        loop.Post({ImageReady, &images, &reading, {{"image", QVariant::fromValue(ImageFile::Create(SyntheticImage()))}}});
        Wait([&] { return ocr.requests == 1; });
        if (duringSpeech) {
            ocr.last = {};
            loop.Post({Recognized, &ocr, &reading, {{"text", "test"}}});
            Wait([&] { return speech.requests == 1; });
        }
        loop.Post({ReadingClip});
        Wait([&] { return duringSpeech ? speech.cancels == 1 : ocr.cancels == 1; });
        Check(reading.CurrentState() == ReadingClipProcessor::State::Cancelling, "OCR/speech cancellation waits for the service terminal event");
        loop.Post({duringSpeech ? Spoken : Recognized, duringSpeech ? static_cast<IProcessor*>(&speech) : &ocr,
            &reading, {{"text", "late result"}}});
        Wait([&] { return reading.CurrentState() == ReadingClipProcessor::State::Idle; });
        Check(reading.LastError() == "reading cancelled" && speech.requests == (duringSpeech ? 1 : 0), "late completion cannot resume cancelled reading");
        loop.Post({ReadingClip}); Wait([&] { return images.requests == 2; });
        loop.Post({Failed, &images, &reading, {{"error", "finish retry"}}}); Drain();
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
        OcrProcessor ocr(loop, std::move(client)); Sink sink(loop);
        auto image = ImageFile::Create(SyntheticImage()); const auto path = image->Path();
        loop.Post({Recognize, &sink, &ocr, {{"image", QVariant::fromValue(image)}}}); image.clear();
        if (QString::fromLatin1(mode) == "hang") {
            Wait([&] { return ocr.Client().Status().value("state") == "pending"; });
            loop.Post({Cancel, &sink, &ocr});
        }
        Wait([&] { return sink.received.size() == 1; });
        Check(sink.received[0].sender == &ocr && sink.received[0].type == (QString::fromLatin1(mode).isEmpty() ? Recognized : Failed), "OCR proxy converts wire completion to pointer-routed event");
        Wait([&] { return !ocr.Client().Status().value("running").toBool(); });
        Check(!QFileInfo::exists(path), "OCR temporary file released after worker use");
    }
    qunsetenv("DESKWAIFU_OCR_TEST_MODE");
    {
        qputenv("DESKWAIFU_OCR_TEST_MODE", "text");
        ProcessorEventLoop loop;
        ImageProcessor images(loop, [] { return SyntheticImage(); });
        OcrProcessor ocr(loop, std::make_unique<OcrClient>(app.arguments()[1], 1000, 50));
        Capability speech(loop, Speak);
        ReadingClipProcessor reading(loop, images, ocr, speech);
        loop.Post({ReadingClip});
        Wait([&] { return speech.requests == 1; });
        Check(speech.last.data.value("text") == "synthetic clipboard text", "complete image-worker to OCR-process to speech-event chain");
        loop.Post({Spoken, &speech, &reading});
        Wait([&] { return reading.CurrentState() == ReadingClipProcessor::State::Idle; });
    }
    qunsetenv("DESKWAIFU_OCR_TEST_MODE");
    {
        ProcessorEventLoop loop;
        MiniMaxSpeechSource source(QString{}); AudioPlayer audio; SpeechPlayer player(source, audio);
        SpeechProcessor speech(loop, player, [] {}); Sink sink(loop);
        loop.Post({Speak, &sink, &speech, {{"text", "test without credentials"}}});
        Wait([&] { return sink.received.size() == 1; });
        Check(sink.received[0].type == Failed && !sink.received[0].data.value("error").toString().isEmpty(), "speech failure returned as event without cloud or playback");
    }
    {
        QTemporaryDir runtime; Check(runtime.isValid(), "private IPC runtime"); qputenv("XDG_RUNTIME_DIR", runtime.path().toUtf8());
        Commands commands; LAppIPC ipc; ipc.SetCommandService(commands); Check(ipc.Initialize(), "initialize private Unix socket");
        ProcessorEventLoop loop; IpcProcessor ipcEvents(loop, ipc);
        QLocalSocket socket; socket.connectToServer(QString::fromStdString(ipc.GetSocketPath()));
        Check(socket.waitForConnected(1000), "connect private IPC client");
        ipcEvents.SetPaused(true);
        QByteArray requests; for (int i = 0; i < 40; ++i) requests += "{}\n";
        socket.write(requests); socket.flush(); Drain();
        Check(commands.count == 0, "paused IPC must not run inside output replacement");
        ipcEvents.SetPaused(false);
        Wait([&] { return commands.count == 40; });
        QByteArray replies;
        Wait([&] { replies += socket.readAll(); return replies.count('\n') == 40; });
        Check(commands.count == 40, "buffered commands drain beyond per-poll budget without any render frames");
        commands.large = true;
        socket.write("{}\n{}\n"); socket.flush(); replies.clear();
        Wait([&] { replies += socket.readAll(); return replies.count('\n') == 2; });
        Check(replies.size() == 2 * (128 * 1024 + 1), "write readiness flushes replies beyond transport write budget");
        socket.disconnectFromServer();
        Wait([&] { return commands.disconnected == 1; });
    }
    std::cout << "Stateful clipboard reading, capability adapters and readiness-driven IPC tests passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
