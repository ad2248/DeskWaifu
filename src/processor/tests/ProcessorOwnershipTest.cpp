#include "processor/command/CommandProcessor.hpp"
#include "processor/ipc/IpcProcessor.hpp"
#include "processor/interaction/InteractionProcessor.hpp"
#include "processor/input/InputListener.hpp"
#include "processor/image/ImageProcessor.hpp"
#include "processor/ocr/OcrProcessor.hpp"
#include "processor/reading_clip/ReadingClipProcessor.hpp"
#include "processor/Events.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>

namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void Wait(const std::function<bool()>& done)
{
    QElapsedTimer timer; timer.start();
    while (!done() && timer.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    Check(done(), "event completion timed out");
}
void Drain() { for (int i = 0; i < 50; ++i) QCoreApplication::processEvents(); }
void Send(QLocalSocket& socket, QJsonObject request)
{
    socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n'); socket.flush();
}
QJsonObject Receive(QLocalSocket& socket)
{
    Wait([&] { return socket.canReadLine(); });
    return QJsonDocument::fromJson(socket.readLine()).object();
}
QJsonObject Request(QLocalSocket& socket, QJsonObject request) { Send(socket, request); return Receive(socket); }

class Window final : public DesktopWindow
{
public:
    IDesktopWindowEvents* events = nullptr;
    bool grabbed = false, closed = false;
    bool Initialize(int, int, IDesktopWindowEvents& sink) override { events = &sink; return true; }
    bool Run(const std::function<bool()>&) override { throw std::runtime_error("test must not render"); }
    bool MakeCurrent() override { return false; }
    void Shutdown() override { closed = true; }
    bool Available() const override { return !closed; }
    int Width() const override { return 800; }
    int Height() const override { return 600; }
    unsigned Framebuffer() const override { return 0; }
    bool SetInputRegion(const std::vector<ModelRegion>&) override { return true; }
    bool KeyboardGrabbed() const override { return grabbed; }
    ModelKeyboardGrabState::GrabResult SetKeyboardGrab(bool value) override
    { grabbed = value; return ModelKeyboardGrabState::GrabResult::Changed; }
    bool OutputNeedsRebind() const override { return false; }
    void ReconcileOutputs() override {}
    std::vector<OutputSnapshot> Outputs() const override { return {{1, "test", 0, 0, 800, 600}}; }
    OutputId CurrentOutput() const override { return 1; }
    bool SwitchOutput(OutputId) override { return true; }
};

class Model final : public Processor
{
public:
    using Processor::Processor;
    IProcessor* pending = nullptr;
    ModelMetadata metadata;
    int selections = 0;
    void Publish()
    {
        metadata.names = {QStringLiteral("one"), QStringLiteral("two")};
        metadata.hotkeys = {{"ok", true}, {"model_token", QString::number(metadata.index)}};
        Send("model.state", nullptr, {{"metadata", QVariant::fromValue(metadata)}});
    }
    void Complete()
    {
        metadata.index = 1; Publish();
        Send("model.result", pending, {{"reply", QJsonObject{{"ok", true}}}}); pending = nullptr;
    }
    bool HandleEvent(const ProcessorEvent& event) override
    {
        if (event.type == "model.hit_test.request") {
            Send("model.hit_test.ready", event.sender, {{"callback", QVariant::fromValue(ModelHitTest{})}}); return true;
        }
        if (event.type != "model.operation") return false;
        const auto command = QJsonDocument::fromJson(event.data.value("json").toString().toUtf8()).object().value("command").toString();
        if (command == "set_model") { ++selections; pending = event.sender; }
        else Send("model.result", event.sender, {{"reply", QJsonObject{{"ok", true}}}});
        return true;
    }
};

class Speech final : public Processor
{
public:
    using Processor::Processor;
    int spoken = 0;
    bool HandleEvent(const ProcessorEvent& event) override
    {
        if (event.type != ProcessorEvents::Speak) return false;
        ++spoken;
        Check(event.data.value("text").toString() == "synthetic clipboard text", "reading must use recognized text");
        Send(ProcessorEvents::Spoken, event.sender); return true;
    }
};
class Samples final : public Processor
{
public:
    using Processor::Processor;
    int native = 0;
    bool HandleEvent(const ProcessorEvent& event) override
    {
        if (event.type == "input.native_move") ++native;
        return false;
    }
};
}

int main(int argc, char** argv)
try {
    QCoreApplication app(argc, argv);
    Check(argc == 2, "expected OCR fixture");
    QTemporaryDir directory; Check(directory.isValid(), "private runtime");
    qputenv("XDG_RUNTIME_DIR", directory.path().toUtf8());
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/deskwaifu-ownership-test");
    qputenv("DESKWAIFU_OCR_TEST_MODE", "text");
    LAppConfig config; config.sourcePath = (directory.path() + "/config.json").toStdString();
    QFile file(QString::fromStdString(config.sourcePath)); Check(file.open(QIODevice::WriteOnly), "config fixture");
    file.write("{\"model_output\":\"test\"}"); file.close();
    ProcessorEventLoop loop; Samples samples(loop); Model model(loop);
    auto backend = std::make_unique<Window>(); auto* window = backend.get();
    auto exchange = std::make_shared<ModelExchange>();
    InteractionProcessor interaction(loop, model, std::move(backend), config, exchange);
    InputListener input(loop, interaction); CommandProcessor commands(loop);
    int reads = 0;
    ImageProcessor images(loop, [&] { ++reads; QImage image(4, 4, QImage::Format_ARGB32); image.fill(Qt::blue); return image; });
    OcrProcessor ocr(loop, std::make_unique<OcrClient>(app.arguments()[1], 1000, 20));
    Speech speech(loop); ReadingClipProcessor reading(loop, images, ocr, speech);
    auto ipc = std::make_unique<IpcProcessor>(loop, std::make_unique<LAppIPC>(), commands);
    Check(interaction.Start(800, 600) && ipc->Start(), "start real ownership graph");
    model.metadata.index = 0; model.Publish(); Drain();

    // 真实窗口事件 -> 内部合并槽 -> InputListener -> Interaction，无帧/GL。
    for (int i = 0; i < 10000; ++i) window->events->PointerMoved(i, 100);
    Drain();
    ModelRenderState gaze; gaze.width = 800; gaze.height = 600;
    const auto output = window->Outputs().front();
    interaction.ApplyLook(gaze, &output, .5f, .5f, 1);
    Check(samples.native == 1 && gaze.hasLook && gaze.lookX > 0, "native motion coalesces and actual listener delivers the final position");

    QLocalSocket first, second;
    const auto path = directory.path() + "/deskwaifu.sock";
    first.connectToServer(path); second.connectToServer(path);
    Check(first.waitForConnected(1000) && second.waitForConnected(1000), "private IPC connections");
    Check(Request(second, {{"command", "get_status"}}).value("model_index") == 0, "metadata state cache");
    Check(Request(second, {{"command", "begin_transform_edit"}}).value("editing").toBool(), "GUI owns edit transaction");
    Check(!Request(first, {{"command", "set_model"}, {"index", 1}}).value("ok").toBool(), "real edit guard before model action");
    Check(model.selections == 0, "rejected edit does not reach model");
    Check(Request(second, {{"command", "cancel_transform_edit"}}).value("ok").toBool(), "cancel transaction");
    Check(Request(second, {{"command", "set_mouth_y"}, {"stream_source", true}, {"value", .4}}).value("ok").toBool(), "stream control uses the GUI owner");
    {
        QLocalSocket transient; transient.connectToServer(path);
        Check(transient.waitForConnected(1000), "transient control connection");
        Check(Request(transient, {{"command", "set_look"}, {"stream_source", true}, {"x", .2}, {"y", -.3}}).value("ok").toBool(), "separate stream control source");
        transient.disconnectFromServer();
        Wait([&] { return !Request(second, {{"command", "get_control_status"}}).value("has_look").toBool(); });
        Check(Request(second, {{"command", "get_control_status"}}).value("has_mouth").toBool(), "disconnect releases only its own controls");
    }
    Send(first, {{"command", "set_model"}, {"index", 1}});
    Send(first, {{"command", "get_current_model"}});
    Wait([&] { return model.pending != nullptr; });
    Check(::shutdown(static_cast<int>(first.socketDescriptor()), SHUT_WR) == 0, "half-close write end");
    Check(!first.canReadLine(), "model result must not be fabricated on enqueue");
    Check(Request(second, {{"command", "get_status"}}).value("model_index") == 0, "other domain progresses during slow model operation");
    Check(Request(second, {{"command", "get_ocr_status"}}).value("ok").toBool(), "OCR API via events");
    model.Complete();
    Check(Receive(first).value("ok").toBool(), "actual asynchronous model result");
    Check(Receive(first).value("model") == "two", "per-connection pipeline and state ordering after half-close");

    // 原 CLI OCR 接受/查询/取消协议全部经过真实路由，沿用 helper 的既有 job_id。
    QImage externalImage(4, 4, QImage::Format_ARGB32); externalImage.fill(Qt::red);
    const auto externalFile = ImageFile::Create(externalImage);
    Check(bool(externalFile), "private external OCR image");
    const auto accepted = Request(second, {{"command", "ocr_recognize"}, {"image", externalFile->Path()}});
    const auto job = accepted.value("job_id").toString();
    Check(accepted.value("ok").toBool() && !job.isEmpty(), "external OCR accepted with original job id");
    QJsonObject recognized;
    Wait([&] {
        recognized = Request(second, {{"command", "get_ocr_result"}, {"job_id", job}});
        return recognized.value("state") != "pending";
    });
    Check(recognized.value("response").toObject().value("ok").toBool(), "external OCR actual completion retained");
    Wait([&] { return !Request(second, {{"command", "get_ocr_status"}}).value("running").toBool(); });
    qputenv("DESKWAIFU_OCR_TEST_MODE", "hang");
    const auto hanging = Request(second, {{"command", "ocr_recognize"}, {"image", externalFile->Path()}});
    Check(hanging.value("ok").toBool(), "external cancellable OCR accepted");
    Check(Request(second, {{"command", "cancel_ocr"}, {"job_id", hanging.value("job_id")}}).value("ok").toBool(), "external OCR cancellation protocol");
    Wait([&] { return !Request(second, {{"command", "get_ocr_status"}}).value("running").toBool(); });
    qputenv("DESKWAIFU_OCR_TEST_MODE", "text");

    Check(Request(second, {{"command", "reading_clip"}}).value("queued").toBool(), "reading event queued");
    Wait([&] { return speech.spoken == 1; });
    Check(Request(second, {{"command", "get_reading_clip_status"}}).value("state") == "idle", "full synthetic reading path");
    Check(reads == 1, "only synthetic clipboard dependency was read");
    second.write("{\"command\":\"quit\",\"command\":\"get_status\"}\n"); second.flush();
    Check(!Receive(second).value("ok").toBool(), "invalid duplicate-key quit must not close the GUI");
    Check(Request(second, {{"command", "get_status"}}).value("ok").toBool(), "GUI remains live after invalid quit");

    // 对端/传输先销毁后完成：领域结果不能调用已销毁传输的回调或复用 fd。
    Send(second, {{"command", "set_model"}, {"index", 1}});
    Wait([&] { return model.pending != nullptr; });
    QLocalSocket queued; queued.connectToServer(path);
    Check(queued.waitForConnected(1000), "queued model connection");
    Send(queued, {{"command", "set_model"}, {"index", 0}}); Drain();
    const int beforeClose = model.selections;
    window->events->SurfaceClosed();
    second.abort(); ipc.reset(); model.Complete(); Drain();
    Check(model.selections == beforeClose, "queued model actions cannot run after the window closes");
    commands.Stop(); input.Stop(); interaction.Stop(); images.Stop(); ocr.Stop();
    interaction.ShutdownWindow();
    Check(window->closed, "window closes after sources and services stop");
    std::cout << "Processor-owned GUI/IPC, async domain routing, half-close and synthetic reading passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
