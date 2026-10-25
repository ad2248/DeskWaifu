#include <GL/glew.h>
#include "processor/model/ModelProcessor.hpp"
#include "processor/model/LAppModel.hpp"
#include "processor/model/LAppLive2DManager.hpp"
#include "ModelProcessorTestHelpers.hpp"
#include "processor/interaction/window/DesktopWindow.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <atomic>
#include <iostream>

#define CHECK(c) do { if (!(c)) { std::cerr << __LINE__ << ": " << #c << '\n'; return 1; } } while (false)

// 此工作线程集成测试不使用所链接的应用组装入口。
std::unique_ptr<DesktopWindow> CreateDesktopWindow() { return {}; }

namespace {
struct Observation
{
    int updates = 0, consecutiveDrags = 0;
    bool pendingDrag = false, appliedDrag = false;
    float x = 0, y = 0;
    ModelFrameInput input;
};
std::mutex observationMutex;
Observation observation;
Observation Observed()
{
    std::lock_guard<std::mutex> lock(observationMutex); return observation;
}
bool WaitUntil(const std::function<bool()>& ready)
{
    QElapsedTimer deadline; deadline.start();
    while (!ready() && deadline.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    return ready();
}
}

// 只观察真实入口，仍调用正式实现；不在测试中复制帧调度或输入转发逻辑。
extern "C" void __real__ZNK17LAppLive2DManager6OnDragEff(const LAppLive2DManager*, float, float);
extern "C" void __wrap__ZNK17LAppLive2DManager6OnDragEff(const LAppLive2DManager* manager, float x, float y)
{
    __real__ZNK17LAppLive2DManager6OnDragEff(manager, x, y);
    std::lock_guard<std::mutex> lock(observationMutex);
    observation.pendingDrag = true; observation.x = x; observation.y = y;
}
extern "C" void __real__ZN9LAppModel6UpdateERK15ModelFrameInput(LAppModel*, const ModelFrameInput&);
extern "C" void __wrap__ZN9LAppModel6UpdateERK15ModelFrameInput(LAppModel* model, const ModelFrameInput& input)
{
    __real__ZN9LAppModel6UpdateERK15ModelFrameInput(model, input);
    std::lock_guard<std::mutex> lock(observationMutex);
    ++observation.updates; observation.input = input;
    observation.appliedDrag = observation.pendingDrag; observation.pendingDrag = false;
    observation.consecutiveDrags = observation.appliedDrag ? observation.consecutiveDrags + 1 : 0;
}

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
    ModelHitTest hitTest;
    bool HandleEvent(const ProcessorEvent& event) override
    {
        if (event.type == "model.hit_test.ready") {
            hitTest = event.data.value("callback").value<ModelHitTest>(); return true;
        }
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
    ModelProcessor processor(loop, std::make_shared<ModelExchange>());
    Calls calls;
    class MetadataSink final : public Processor
    {
    public:
        using Processor::Processor;
        int changes = 0;
        ModelMetadata metadata;
        bool HandleEvent(const ProcessorEvent& event) override
        {
            if (event.type != "model.state") return false;
            const auto next = event.data.value("metadata").value<ModelMetadata>();
            if (metadata.index >= 0 && next.index != metadata.index) ++changes;
            metadata = next;
            return false;
        }
    } metadata(loop);
    CHECK(processor.Start(std::unique_ptr<ModelRenderTarget>(new TrackedTarget(calls)),
        {"", directory.path().toStdString(), "", {}, -1}, 64, 48));
    CHECK(ModelMetadataOf(processor).names.size() == 2 && ModelMetadataOf(processor).index == 0);

    // 刻意不处理 GUI 派发器；模型更新和渲染仍必须继续执行。
    QThread::msleep(150);
    CHECK(calls.frames >= 2 && ModelFrameOf(processor) && ModelFrameOf(processor)->rgba.size() == 64 * 48 * 4);
    const auto first = ModelFrameOf(processor);
    Receiver hitReceiver(loop);
    CHECK(loop.Post({"model.hit_test.request", &hitReceiver, &processor, {}}));
    QElapsedTimer handshake; handshake.start();
    while (!hitReceiver.hitTest && handshake.elapsed() < 5000) QCoreApplication::processEvents();
    CHECK(hitReceiver.hitTest);
    const auto originalToken = ModelMetadataOf(processor).hotkeys.value("model_token").toString().toStdString();
    const auto hit = hitReceiver.hitTest({20, 10, 64, 48, originalToken});
    CHECK(hit.available && hit.parts == std::vector<std::string>{"Head"});
    CHECK(!hitReceiver.hitTest({20, 10, 32, 48, originalToken}).available);
    CHECK(!hitReceiver.hitTest({20, 10, 64, 48, "stale"}).available);
    // 直接覆盖生产 ModelRuntime::Render：每帧重施视线、编辑抑制、外部输入转发。
    ModelRenderState gaze; gaze.width = 64; gaze.height = 48; gaze.clip = {0, 0, 64, 48};
    gaze.modelToken = originalToken; gaze.hasLook = true; gaze.lookX = .5f; gaze.lookY = -.3f;
    gaze.controls = {true, .7f, true, -.8f, .3f};
    SetModelState(processor, gaze);
    CHECK(WaitUntil([&] {
        const auto value = Observed();
        return value.appliedDrag && value.x == .5f && value.y == -.3f && value.input.hasLook &&
            value.input.lookX == -.8f && value.input.lookY == .3f && value.input.hasMouth && value.input.mouth == .7f;
    }));
    const int gazeUpdates = Observed().updates;
    CHECK(AwaitModel(processor)["ok"].toBool()); // reset_drag 后，静止光标仍须在后续帧恢复。
    CHECK(WaitUntil([&] {
        const auto value = Observed();
        return value.updates >= gazeUpdates + 3 && value.consecutiveDrags >= 3 && value.x == .5f && value.y == -.3f;
    }));
    gaze.editing = true; SetModelState(processor, gaze);
    CHECK(WaitUntil([&] { const auto value = Observed(); return !value.appliedDrag && value.input.hasLook; }));
    gaze.editing = false; gaze.controls = {}; SetModelState(processor, gaze);
    CHECK(WaitUntil([&] { const auto value = Observed(); return value.appliedDrag && !value.input.hasLook && !value.input.hasMouth; }));
    gaze.modelToken = "stale"; gaze.controls = {true, .9f, true, .9f, .9f}; SetModelState(processor, gaze);
    const int staleUpdates = Observed().updates;
    CHECK(WaitUntil([&] { const auto value = Observed(); return value.updates > staleUpdates && !value.input.hasLook && !value.input.hasMouth; }));
    const int before = calls.frames;
    ModelRenderState state; state.width = 32; state.height = 24;
    state.clip = {0, 0, 32, 24}; state.app.hidden = true;
    for (int i = 0; i < 10000; ++i) SetModelState(processor, state); // 替换状态，不排入一万帧。
    QThread::msleep(100);
    const auto hidden = ModelFrameOf(processor);
    CHECK(hidden && hidden->width == 32 && hidden->height == 24 && hidden->regions.empty());
    CHECK(first->width == 64 && first->rgba.size() == 64 * 48 * 4); // 保留帧不可变。
    CHECK(calls.frames > before && calls.frames < before + 50);
    for (auto byte : hidden->rgba) CHECK(byte == 0);
    const int hiddenUpdates = Observed().updates;
    QThread::msleep(50);
    CHECK(Observed().updates == hiddenUpdates); // 隐藏后仍产出透明帧，但不更新模型。
    gaze.modelToken = originalToken; gaze.controls = {}; SetModelState(processor, gaze);
    CHECK(WaitUntil([&] {
        const auto frame = ModelFrameOf(processor);
        const auto value = Observed();
        return frame && !frame->hidden && frame->width == 64 && value.updates > hiddenUpdates && value.appliedDrag;
    }));

    CHECK(!AwaitModel(processor, {{"command", "set_model"}, {"index", -1}})["ok"].toBool());
    CHECK(ModelMetadataOf(processor).index == 0 && metadata.changes == 0);
    CHECK(AwaitModel(processor, {{"command", "set_model"}, {"index", 0}})["ok"].toBool());
    CHECK(metadata.changes == 0);
    CHECK(AwaitModel(processor, {{"command", "set_model"}, {"index", 1}})["ok"].toBool());
    CHECK(metadata.changes == 1 && ModelMetadataOf(processor).index == 1);
    CHECK(!AwaitModel(processor, {{"command", "set_expression"}, {"id", "missing"}})["ok"].toBool());
    CHECK(!AwaitModel(processor, {{"command", "do_motion"}, {"group", "missing"}})["ok"].toBool());
    CHECK(!AwaitModel(processor, {{"command", "trigger_hotkey"}, {"model_token", "stale"}, {"id", "missing"}})["ok"].toBool());
    CHECK(!AwaitModel(processor, {{"command", "random_expression"}, {"model_token", "stale"}})["ok"].toBool());

    Receiver receiver(loop);
    CHECK(loop.Post({"model.operation", &receiver, &processor, {{"json", R"({"command":"reset_drag"})"}}}));
    AwaitModel(processor);
    for (int i = 0; i < 4; ++i) QCoreApplication::processEvents();
    CHECK(receiver.received && receiver.sender == &processor);
    CHECK(processor.Stop() && processor.Stop());
    CHECK(!hitReceiver.hitTest({20, 10, 64, 48, originalToken}).available);
    CHECK(!calls.wrongThread && calls.destroyedOnGui && calls.shutdowns == 1);
    CHECK(!AwaitModel(processor)["ok"].toBool());

    // 初始化失败后必须在同一工作线程清理，并保证 Cubism 可以再次使用。
    ModelProcessor failed(loop, std::make_shared<ModelExchange>());
    Calls failedCalls;
    CHECK(!failed.Start(std::unique_ptr<ModelRenderTarget>(new TrackedTarget(failedCalls)),
        {"", "/nonexistent/deskwaifu-test-models", "", {}, -1}, 16, 16));
    CHECK(!failedCalls.wrongThread && failedCalls.destroyedOnGui && failedCalls.shutdowns == 1);
    ModelProcessor recovered(loop, std::make_shared<ModelExchange>());
    Calls recoveredCalls;
    CHECK(recovered.Start(std::unique_ptr<ModelRenderTarget>(new TrackedTarget(recoveredCalls)),
        {"", directory.path().toStdString(), "", {}, -1}, 16, 16));
    Receiver recoveredReceiver(loop);
    CHECK(loop.Post({"model.hit_test.request", &recoveredReceiver, &recovered, {}}));
    handshake.restart();
    while (!recoveredReceiver.hitTest && handshake.elapsed() < 5000) QCoreApplication::processEvents();
    CHECK(recoveredReceiver.hitTest);
    const auto oldCallback = recoveredReceiver.hitTest;
    const auto oldToken = ModelMetadataOf(recovered).hotkeys.value("model_token").toString().toStdString();
    recoveredCalls.failBind = true;
    QThread::msleep(100);
    CHECK(!ModelErrorOf(recovered).isEmpty() && !AwaitModel(recovered)["ok"].toBool());
    CHECK(!oldCallback({8, 8, 16, 16, oldToken}).available);
    CHECK(recovered.Stop() && !recoveredCalls.wrongThread && recoveredCalls.shutdowns == 1);
    recoveredCalls.failBind = false;
    CHECK(recovered.Start(std::unique_ptr<ModelRenderTarget>(new TrackedTarget(recoveredCalls)),
        {"", directory.path().toStdString(), "", {}, -1}, 16, 16));
    CHECK(ModelErrorOf(recovered).isEmpty() && AwaitModel(recovered)["ok"].toBool());
    CHECK(!oldCallback({8, 8, 16, 16, oldToken}).available); // 重启不能复活旧回调。
    recoveredReceiver.hitTest = {};
    CHECK(loop.Post({"model.hit_test.request", &recoveredReceiver, &recovered, {}}));
    handshake.restart();
    const auto newToken = ModelMetadataOf(recovered).hotkeys.value("model_token").toString().toStdString();
    while (handshake.elapsed() < 5000) {
        QCoreApplication::processEvents();
        if (recoveredReceiver.hitTest && recoveredReceiver.hitTest({8, 8, 16, 16, newToken}).available) break;
        QThread::msleep(1);
    }
    CHECK(recoveredReceiver.hitTest && recoveredReceiver.hitTest({8, 8, 16, 16, newToken}).available);
    CHECK(recovered.Stop() && recoveredCalls.shutdowns == 2);
    return 0;
}
