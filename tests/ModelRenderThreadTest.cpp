#include "processor/model/ModelProcessor.hpp"
#include "ModelProcessorTestHelpers.hpp"
#include "DesktopWindow.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <atomic>
#include <iostream>

#define CHECK(c) do { if (!(c)) { std::cerr << __LINE__ << ": " << #c << '\n'; return 1; } } while (false)

// The linked application composition root is not used by this worker integration test.
std::unique_ptr<DesktopWindow> CreateDesktopWindow() { return {}; }

struct Calls
{
    QThread* gui = QThread::currentThread();
    QThread* worker = nullptr;
    std::atomic<int> frames{0}, shutdowns{0};
    std::atomic<bool> wrongThread{false}, destroyedOnGui{false};
    std::atomic<bool> failBind{false};
};

class TrackedTarget final : public ModelRenderTarget
{
public:
    explicit TrackedTarget(Calls& calls) : _calls(calls), _target(CreateEglModelRenderTarget("auto")) {}
    ~TrackedTarget() override { _calls.destroyedOnGui = QThread::currentThread() == _calls.gui; }
    bool Prepare(QThread* thread) override { _calls.worker = thread; return _target->Prepare(thread); }
    bool Initialize() override { Check(); return _target->Initialize(); }
    bool MakeCurrent() override { Check(); return _target->MakeCurrent(); }
    bool Bind(int w, int h) override { Check(); return !_calls.failBind && _target->Bind(w, h); }
    unsigned Framebuffer() const override { Check(); return _target->Framebuffer(); }
    bool Read(std::vector<unsigned char>& rgba) override { Check(); ++_calls.frames; return _target->Read(rgba); }
    bool Shutdown() override { Check(); ++_calls.shutdowns; return _target->Shutdown(); }
private:
    void Check() const { if (QThread::currentThread() != _calls.worker || _calls.worker == _calls.gui) _calls.wrongThread = true; }
    Calls& _calls;
    std::unique_ptr<ModelRenderTarget> _target;
};

class Receiver final : public Processor
{
public:
    using Processor::Processor;
    bool received = false;
    IProcessor* sender = nullptr;
    bool HandleEvent(const ProcessorEvent& event) override
    {
        if (event.type != "model.result") return false;
        received = event.receiver == this && event.data.value("reply").toJsonObject()["ok"].toBool();
        sender = event.sender;
        return true;
    }
};

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    CHECK(directory.isValid() && QDir(directory.path()).mkdir("model"));
    CHECK(QFile::link(QStringLiteral(SDK_RESOURCES "/Haru/Haru.moc3"), directory.path() + "/model/test.moc3"));
    for (const auto* name : {"test", "z-other"}) {
        QFile file(directory.path() + "/model/" + name + ".model3.json");
        CHECK(file.open(QIODevice::WriteOnly));
        CHECK(file.write(R"({"Version":3,"FileReferences":{"Moc":"test.moc3"}})") > 0);
    }
    ProcessorEventLoop loop;
    ModelProcessor processor(loop);
    Calls calls;
    int changes = 0;
    processor.SetModelChanged([&] { ++changes; });
    CHECK(processor.Start(std::unique_ptr<ModelRenderTarget>(new TrackedTarget(calls)),
        {"", directory.path().toStdString(), "", {}, -1}, 64, 48));
    CHECK(processor.Metadata().names.size() == 2 && processor.Metadata().index == 0);

    // GUI dispatcher is deliberately not serviced. Model update/render must still run.
    QThread::msleep(150);
    CHECK(calls.frames >= 2 && processor.Frame() && processor.Frame()->rgba.size() == 64 * 48 * 4);
    const auto first = processor.Frame();
    const int before = calls.frames;
    ModelRenderState state; state.width = 32; state.height = 24;
    state.clip = {0, 0, 32, 24}; state.app.hidden = true;
    for (int i = 0; i < 10000; ++i) processor.SetState(state); // Replace, do not queue 10,000 frames.
    QThread::msleep(100);
    const auto hidden = processor.Frame();
    CHECK(hidden && hidden->width == 32 && hidden->height == 24 && hidden->regions.empty());
    CHECK(first->width == 64 && first->rgba.size() == 64 * 48 * 4); // Immutable retained frame.
    CHECK(calls.frames > before && calls.frames < before + 50);
    for (auto byte : hidden->rgba) CHECK(byte == 0);

    CHECK(!AwaitModel(processor, {{"command", "set_model"}, {"index", -1}})["ok"].toBool());
    CHECK(processor.Metadata().index == 0 && changes == 0);
    CHECK(AwaitModel(processor, {{"command", "set_model"}, {"index", 0}})["ok"].toBool());
    CHECK(changes == 0);
    CHECK(AwaitModel(processor, {{"command", "set_model"}, {"index", 1}})["ok"].toBool());
    CHECK(changes == 1 && processor.Metadata().index == 1);
    CHECK(!AwaitModel(processor, {{"command", "set_expression"}, {"id", "missing"}})["ok"].toBool());
    CHECK(!AwaitModel(processor, {{"command", "do_motion"}, {"group", "missing"}})["ok"].toBool());
    CHECK(!AwaitModel(processor, {{"command", "trigger_hotkey"}, {"model_token", "stale"}, {"id", "missing"}})["ok"].toBool());

    bool completedOnGui = false;
    CHECK(processor.Submit(R"({"command":"reset_drag"})", [&](QJsonObject reply) {
        completedOnGui = reply["ok"].toBool() && QThread::currentThread() == calls.gui;
    }));
    AwaitModel(processor);
    CHECK(completedOnGui);
    Receiver receiver(loop);
    CHECK(loop.Post({"model.operation", &receiver, &processor, {{"json", R"({"command":"reset_drag"})"}}}));
    AwaitModel(processor);
    for (int i = 0; i < 4; ++i) QCoreApplication::processEvents();
    CHECK(receiver.received && receiver.sender == &processor);
    CHECK(processor.Stop() && processor.Stop());
    CHECK(!calls.wrongThread && calls.destroyedOnGui && calls.shutdowns == 1);
    CHECK(!processor.Submit(R"({"command":"reset_drag"})"));

    // Failed initialization must unwind on the same worker and leave Cubism reusable.
    ModelProcessor failed(loop);
    Calls failedCalls;
    CHECK(!failed.Start(std::unique_ptr<ModelRenderTarget>(new TrackedTarget(failedCalls)),
        {"", "/nonexistent/deskwaifu-test-models", "", {}, -1}, 16, 16));
    CHECK(!failedCalls.wrongThread && failedCalls.destroyedOnGui && failedCalls.shutdowns == 1);
    ModelProcessor recovered(loop);
    Calls recoveredCalls;
    CHECK(recovered.Start(std::unique_ptr<ModelRenderTarget>(new TrackedTarget(recoveredCalls)),
        {"", directory.path().toStdString(), "", {}, -1}, 16, 16));
    recoveredCalls.failBind = true;
    QThread::msleep(100);
    CHECK(!recovered.Error().isEmpty() && !recovered.Submit(R"({"command":"reset_drag"})"));
    CHECK(recovered.Stop() && !recoveredCalls.wrongThread && recoveredCalls.shutdowns == 1);
    recoveredCalls.failBind = false;
    CHECK(recovered.Start(std::unique_ptr<ModelRenderTarget>(new TrackedTarget(recoveredCalls)),
        {"", directory.path().toStdString(), "", {}, -1}, 16, 16));
    CHECK(recovered.Error().isEmpty() && AwaitModel(recovered)["ok"].toBool());
    CHECK(recovered.Stop() && recoveredCalls.shutdowns == 2);
    return 0;
}
