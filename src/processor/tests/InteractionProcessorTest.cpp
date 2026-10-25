#include "processor/interaction/InteractionProcessor.hpp"
#include "processor/input/InputListener.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cmath>
#include <fstream>
#include <iostream>
#include <chrono>

#define CHECK(condition) do { if (!(condition)) { std::cerr << __LINE__ << ": " << #condition << '\n'; return 1; } } while (false)

// 此测试只运行交互策略，不创建窗口或 GL 上下文。
std::unique_ptr<ModelRenderTarget> CreateQtModelRenderTarget() { return {}; }
std::unique_ptr<ModelRenderTarget> CreateEglModelRenderTarget(const std::string&) { return {}; }

namespace {
class Window final : public DesktopWindow
{
public:
    bool grabbed = false;
    bool Initialize(int, int, IDesktopWindowEvents&) override { return true; }
    bool Run(const std::function<bool()>&) override { return true; }
    bool MakeCurrent() override { return true; }
    void Shutdown() override {}
    bool Available() const override { return true; }
    int width = 800, height = 600;
    int Width() const override { return width; }
    int Height() const override { return height; }
    unsigned Framebuffer() const override { return 0; }
    std::unique_ptr<ModelRenderTarget> RenderTarget(const std::string&) override { return {}; }
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
struct Runtime : IApplicationRuntime
{
    int ModelIndex() const override { return 0; }
    int ModelCount() const override { return 1; }
    bool SetHidden(bool) override { return true; }
};
class Model final : public Processor
{
public:
    using Processor::Processor;
    std::vector<QJsonObject> operations;
    bool HandleEvent(const ProcessorEvent& event) override
    {
        if (event.type == "model.hit_test.request") {
            ModelMetadata metadata; metadata.hotkeys = {{"model_token", "test-token"}};
            Send("model.state", nullptr, {{"metadata", QVariant::fromValue(metadata)}});
            ModelHitSnapshot hit; hit.width = 800; hit.height = 600; hit.modelToken = "test-token";
            hit.regions = {{100, 100, 500, 400}}; hit.partsEnabled = true; hit.fallback = true;
            hit.fallbackYScale = -1; hit.fallbackYOffset = 300;
            ModelHitTest callback = [hit](const ModelHitQuery& query) { return hit.Test(query); };
            Send("model.hit_test.ready", event.sender, {{"callback", QVariant::fromValue(callback)}});
            return true;
        }
        if (event.type != "model.operation") return false;
        operations.push_back(QJsonDocument::fromJson(event.data.value("json").toString().toUtf8()).object());
        Send("model.result", event.sender, {{"reply", QJsonObject{{"ok", true}}}});
        return true;
    }
    int Taps() const
    {
        int count = 0;
        for (const auto& operation : operations) if (operation.value("command") == "random_expression") ++count;
        return count;
    }
};
void Wait(int milliseconds)
{
    QEventLoop wait;
    QTimer::singleShot(milliseconds, &wait, &QEventLoop::quit);
    wait.exec();
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    CHECK(directory.isValid());
    LAppConfig config;
    config.sourcePath = (directory.path() + "/config.json").toStdString();
    { std::ofstream file(config.sourcePath); file << "{\"model_scale\":1}"; }
    Runtime runtime; ApplicationService service(runtime);
    TransformEditor settings(service, config);
    CHECK(settings.Initialize({}));
    ModelWindowState mode;
    mode.Send(ModelWindowState::Event::Initialize);
    mode.Send(ModelWindowState::Event::Initialized);
    settings.SetEditChanged([&] {
        mode.Send(!settings.Editing() ? ModelWindowState::Event::NormalMode :
            settings.ScreenEditing() ? ModelWindowState::Event::ScreenMode : ModelWindowState::Event::PanelMode);
    });
    ProcessorEventLoop loop; Model model(loop); Window window;
    class Observer final : public Processor
    {
    public:
        using Processor::Processor;
        int greetings = 0, moves = 0;
        bool HandleEvent(const ProcessorEvent& event) override
        {
            if (event.type == "speech.speak") { ++greetings; return true; }
            if (event.type == "interaction.move_screen") { ++moves; return true; }
            return false;
        }
    } observer(loop);
    auto& greetings = observer.greetings; auto& moves = observer.moves;
    auto makeInteraction = [&] {
        return std::make_unique<InteractionProcessor>(loop, window, mode, service, settings, model);
    };
    auto interaction = makeInteraction();
    Wait(20); // 初始化请求与回调回复均通过统一队列派发。
    auto click = [&](int x, int y) {
        interaction->PointerMoved(x, y);
        interaction->PointerButton(0, true); interaction->PointerButton(0, false);
    };

    // 不运行任何渲染帧；单击由独立 Qt 定时器完成且只派发一次。
    click(300, 200);
    CHECK(model.Taps() == 0);
    Wait(450);
    CHECK(model.Taps() == 1);
    CHECK(model.operations.back().value("model_token") == "test-token");
    Wait(400); CHECK(model.Taps() == 1);

    click(300, 200); click(300, 200);
    Wait(20);
    CHECK(greetings == 1 && window.grabbed);
    Wait(450); CHECK(model.Taps() == 1);
    interaction->Key(1, true); CHECK(!window.grabbed);

    // 远处的第二次点击立即派发第一次，第二次仍独立等待。
    click(300, 200); click(500, 400);
    Wait(20); CHECK(model.Taps() == 2);
    Wait(450); CHECK(model.Taps() == 3);

    // 拖动进入编辑模式，取消旧单击，不重复触发模型点击或问候。
    click(300, 200);
    interaction->PointerButton(0, true);
    interaction->PointerMoved(340, 240);
    CHECK(settings.LocalScreenEditing() && mode.GestureActive());
    interaction->PointerButton(0, false);
    Wait(20);
    CHECK(moves == 1);
    Wait(450); CHECK(model.Taps() == 3 && greetings == 1);
    CHECK(settings.Cancel(settings.Owner()).ok);

    // 面板编辑、输出切换、关闭均撤销待处理单击。
    click(300, 200);
    CHECK(settings.Begin("panel", false, 800, 600).ok);
    Wait(450); CHECK(model.Taps() == 3);
    CHECK(settings.Cancel("panel").ok);
    click(300, 200);
    mode.Send(ModelWindowState::Event::SwitchOutput);
    mode.Send(ModelWindowState::Event::OutputReady);
    Wait(450); CHECK(model.Taps() == 3);

    // 不产生光标事件，仅改变模型变换、视线半径或视口也应重新求值。
    interaction->Position(600, 300);
    ModelRenderState state; state.width = 800; state.height = 600; state.app = service.Snapshot();
    const auto output = window.Outputs()[0];
    interaction->ApplyLook(state, &output, .5f, .5f, 1);
    CHECK(state.hasLook && std::abs(state.lookX - .5f) < .0001f);
    state.app.transform.x = 1;
    interaction->ApplyLook(state, &output, .5f, .5f, 1);
    CHECK(std::abs(state.lookX + .25f) < .0001f);
    interaction->ApplyLook(state, &output, .5f, .5f, .1f);
    CHECK(std::abs(state.lookX + .1f) < .0001f);
    state.app.transform.x = 0; state.width = 1200;
    interaction->ApplyLook(state, &output, .5f, .5f, 1);
    CHECK(state.lookX == 0);
    state.width = 800;
    interaction->Position(700, 400);
    interaction->ApplyLook(state, &output, .5f, .5f, 1);
    CHECK(state.lookX != 0 && state.lookY != 0);
    interaction->ApplyLook(state, &output, .5f, .5f, 0);
    CHECK(state.lookX == 0 && state.lookY == 0);
    state.editing = true;
    interaction->ApplyLook(state, &output, .5f, .5f, 1); CHECK(!state.hasLook);

    // 监听层只投递输入；连续移动合并，但按钮不会越过移动或被合并丢失。
    InputListener listener(loop, *interaction);
    listener.Event("enter", {{"x", 300}, {"y", 200}});
    for (int i = 0; i < 10000; ++i) listener.Move(300, 200);
    CHECK(!interaction->Hover().inside); // 输入尚未派发。
    Wait(20);
    auto hover = interaction->Hover();
    CHECK(hover.available && hover.inside && hover.parts == std::vector<std::string>{"Head"});
    state.width = 800; state.height = 600;
    interaction->PrepareFrame(state, true, .4f, .8f); CHECK(state.opacity == .4f);
    CHECK(interaction->InputRegions(state, {{100, 100, 30, 30}}).size() == 1);
    state.app.hidden = true;
    CHECK(interaction->InputRegions(state, {{100, 100, 30, 30}}).empty());
    state.app.hidden = false;
    listener.Event("leave"); Wait(20);
    interaction->PrepareFrame(state, true, .4f, .8f); CHECK(state.opacity == .8f);
    listener.Move(300, 200);
    listener.Event("button", {{"button", 0}, {"pressed", true}});
    listener.Move(340, 240);
    listener.Event("button", {{"button", 0}, {"pressed", false}});
    Wait(20); CHECK(settings.LocalScreenEditing());
    interaction->PrepareFrame(state, true, .4f, .8f);
    CHECK(state.editing && !state.handles.empty());
    const auto editRegions = interaction->InputRegions(state, {});
    CHECK(editRegions.size() == 1 && editRegions[0].width == 800 && editRegions[0].height == 600);
    CHECK(settings.Cancel(settings.Owner()).ok);
    listener.Event("button", {{"button", 0}, {"pressed", true}});
    listener.SetPaused(true);
    listener.Event("button", {{"button", 0}, {"pressed", true}});
    listener.SetPaused(false); Wait(20);
    CHECK(!interaction->Captured());
    listener.Event("button", {{"button", 0}, {"pressed", true}});
    CHECK(service.SetVisibility(false).ok);
    Wait(20); CHECK(!interaction->Captured());
    CHECK(service.SetVisibility(true).ok);
    listener.Stop();

    // 测试实际 PointerScroll 的屏幕坐标转换，不再调用旧 View 适配器。
    CHECK(service.SetTransform({1, .3f, -.2f}).ok);
    interaction->Position(640, 450);
    const auto beforeScroll = service.Snapshot().transform;
    const float modelX = .8f / beforeScroll.scale - beforeScroll.x;
    const float modelY = -.5f / beforeScroll.scale - beforeScroll.y;
    interaction->PointerScroll(1);
    const auto afterScroll = service.Snapshot().transform;
    CHECK(std::abs(afterScroll.scale - 1.1f) < .00001f);
    CHECK(std::abs(afterScroll.scale * (modelX + afterScroll.x) - .8f) < .00001f);
    CHECK(std::abs(afterScroll.scale * (modelY + afterScroll.y) + .5f) < .00001f);
    window.height = 0;
    interaction->PointerScroll(1);
    CHECK(service.Snapshot().transform.scale == afterScroll.scale);
    CHECK(service.Snapshot().transform.x == afterScroll.x && service.Snapshot().transform.y == afterScroll.y);
    window.height = 600;
    CHECK(settings.Begin("panel", false, 800, 600).ok);
    const auto panelTransform = service.Snapshot().transform;
    interaction->PointerScroll(1);
    CHECK(service.Snapshot().transform.scale == panelTransform.scale);
    CHECK(settings.Cancel("panel").ok);

    // 排队延迟不能把物理时间上分离的两次单击误判为双击。
    const auto now = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    interaction->Position(300, 200);
    const int beforeDelayed = model.Taps();
    interaction->PointerButton(0, true, now - 1000);
    interaction->PointerButton(0, false, now - 1000);
    interaction->PointerButton(0, true, now - 500);
    interaction->PointerButton(0, false, now - 500);
    Wait(20);
    CHECK(model.Taps() == beforeDelayed + 2 && greetings == 1);
    model.operations.clear(); // 下列生命周期检查重新从零开始计数。

    // 已排队的旧超时不能抢先派发取消后新一轮的单击。
    CHECK(loop.Post({"interaction.tap_timeout", interaction.get(), interaction.get(), {}}));
    interaction->Cancel();
    click(300, 200);
    Wait(20); CHECK(model.Taps() == 0);
    interaction->Stop();
    const auto scale = service.Snapshot().transform.scale;
    click(300, 200); interaction->PointerScroll(1);
    CHECK(service.Snapshot().transform.scale == scale && greetings == 1);
    Wait(450); CHECK(model.Taps() == 0);
    interaction.reset();

    // 定时器仍活跃时销毁，且清除尚未派发的自身事件。
    interaction = makeInteraction();
    click(300, 200);
    CHECK(loop.Post({"interaction.tap_timeout", interaction.get(), interaction.get(), {}}));
    interaction.reset();
    Wait(450); CHECK(model.Taps() == 0);

    interaction = makeInteraction();
    click(300, 200);
    mode.Send(ModelWindowState::Event::Close);
    Wait(450); CHECK(model.Taps() == 0);
    mode.Send(ModelWindowState::Event::Released);
    interaction.reset();
    return 0;
}
