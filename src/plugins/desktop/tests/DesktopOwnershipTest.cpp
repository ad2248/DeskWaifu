#include "processor/interaction/InteractionProcessor.hpp"
#include "processor/input/InputListener.hpp"
#include "processor/Events.hpp"
#include "EventLoopFence.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void Wait(const std::function<bool()>& done)
{
    QElapsedTimer timer; timer.start();
    while (!done() && timer.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    Check(done(), "desktop ownership completion timed out");
}
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
    IProcessorPlugin* pending = nullptr;
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
        if (command != "set_model") {
            Send("model.result", event.sender, {{"reply", QJsonObject{{"ok", true}}}});
            return true;
        }
        ++selections; pending = event.sender; return true;
    }
};
class Client final : public Processor
{
public:
    Client(ProcessorEventLoop& loop, InteractionProcessor& interaction) : Processor(loop), _interaction(interaction) {}
    std::vector<QJsonObject> replies;
    bool HandleEvent(const ProcessorEvent& event) override
    {
        if (event.receiver != this) return false;
        if (event.type == "interaction.result") replies.push_back(QJsonDocument::fromJson(event.data.value("json").toString().toUtf8()).object());
        else if (event.type == "model.result") replies.push_back(event.data.value("reply").toJsonObject());
        else return false;
        return true;
    }
    QJsonObject Request(const QString& command, QJsonObject input = {}, const QString& owner = QStringLiteral("gui"))
    {
        const auto before = replies.size();
        SendCommand(command, input, owner);
        Wait([&] { return replies.size() != before; }); return replies.back();
    }
    void SendCommand(const QString& command, QJsonObject input = {}, const QString& owner = QStringLiteral("gui"))
    {
        input.insert("command", command);
        Send(command == "set_model" ? "interaction.model" : "interaction.command", &_interaction,
            {{"json", QString::fromUtf8(QJsonDocument(input).toJson(QJsonDocument::Compact))}, {"connection", owner}});
    }
private:
    InteractionProcessor& _interaction;
};
class Samples final : public Processor
{
public:
    using Processor::Processor;
    int native = 0, spoken = 0;
    bool HandleEvent(const ProcessorEvent& event) override
    {
        if (event.type == "input.native_move") ++native;
        if (event.type == ProcessorEvents::Speak) ++spoken;
        return false;
    }
};
}
int main(int argc, char** argv)
try {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory; Check(directory.isValid(), "private runtime");
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/deskwaifu-ownership-test");
    LAppConfig config; config.sourcePath = (directory.path() + "/config.json").toStdString();
    QFile file(QString::fromStdString(config.sourcePath)); Check(file.open(QIODevice::WriteOnly), "config fixture");
    file.write("{\"model_output\":\"test\"}"); file.close();
    ProcessorEventLoop loop; Samples samples(loop); Model model(loop);
    auto backend = std::make_unique<Window>(); auto* window = backend.get();
    auto exchange = std::make_shared<ModelExchange>();
    InteractionProcessor interaction(loop, model, std::move(backend), config, exchange);
    InputListener input(loop, interaction); Client client(loop, interaction);
    Check(interaction.Start(800, 600), "start real desktop ownership graph");
    auto drain = [&] { Check(AwaitProcessorEvents(loop), "desktop event fence"); };
    model.metadata.index = 0; model.Publish(); drain();
    for (int i = 0; i < 10000; ++i) window->events->PointerMoved(i, 100);
    drain();
    ModelRenderState gaze; gaze.width = 800; gaze.height = 600;
    const auto output = window->Outputs().front();
    Wait([&] { interaction.ApplyLook(gaze, &output, .5f, .5f, 1); return gaze.hasLook; });
    Check(samples.native == 1 && gaze.hasLook && gaze.lookX > 0, "native motion coalesces and actual listener delivers the final position");
    Check(client.Request("get_status").value("model_index") == 0, "metadata state cache");
    auto click = [&] {
        window->events->PointerMoved(300, 200);
        window->events->PointerButton(0, true); window->events->PointerButton(0, false); drain();
    };
    QElapsedTimer clicks; clicks.start(); click();
    Check(client.Request("toggle_hidden").value("hidden").toBool(), "hide pending single click");
    Check(!client.Request("toggle_hidden").value("hidden").toBool(), "show after pending click"); click();
    Check(clicks.elapsed() < ModelDoubleClick::Delay, "regression clicks must fall within double-click interval");
    Check(!window->grabbed && samples.spoken == 0, "hide/show separates clicks without keyboard capture or greeting");
    Check(client.Request("begin_transform_edit").value("editing").toBool(), "GUI owns edit transaction");
    Check(!client.Request("set_model", {{"index", 1}}, "other").value("ok").toBool(), "real edit guard before model action");
    Check(model.selections == 0, "rejected edit does not reach model");
    Check(client.Request("cancel_transform_edit").value("ok").toBool(), "cancel transaction");
    Check(client.Request("set_mouth_y", {{"stream_source", true}, {"value", .4}}).value("ok").toBool(), "stream control uses the GUI owner");
    Check(client.Request("set_look", {{"stream_source", true}, {"x", .2}, {"y", -.3}}, "transient").value("ok").toBool(), "separate stream control source");
    loop.Post({"command.disconnected", nullptr, nullptr, {{"connection", "transient"}}}); drain();
    const auto controls = client.Request("get_control_status");
    Check(!controls.value("has_look").toBool() && controls.value("has_mouth").toBool(), "disconnect releases only its own controls");
    client.SendCommand("set_model", {{"index", 1}});
    Wait([&] { return model.pending != nullptr; });
    const int beforeClose = model.selections;
    window->events->SurfaceClosed();
    model.Complete(); drain();
    Check(!client.Request("set_model", {{"index", 0}}).value("ok").toBool(), "closed window rejects queued model actions");
    Check(model.selections == beforeClose, "queued model actions cannot run after the window closes");
    input.Stop(); interaction.Stop(); interaction.ShutdownWindow();
    Check(window->closed, "window closes after sources and services stop");
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
