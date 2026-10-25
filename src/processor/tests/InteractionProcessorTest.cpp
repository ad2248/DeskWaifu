#include "processor/interaction/InteractionProcessor.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <cmath>
#include <fstream>
#include <iostream>

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
    int Width() const override { return 800; }
    int Height() const override { return 600; }
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
        if (event.type != "model.operation") return false;
        operations.push_back(QJsonDocument::fromJson(event.data.value("json").toString().toUtf8()).object());
        Send("model.result", event.sender, {{"reply", QJsonObject{{"ok", true}}}});
        return true;
    }
    int Taps() const
    {
        int count = 0;
        for (const auto& operation : operations) if (operation.value("command") == "tap_model") ++count;
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
    int greetings = 0, moves = 0;
    auto makeInteraction = [&] {
        return std::make_unique<InteractionProcessor>(loop, window, mode, service, settings,
            model, [] { ModelMetadata value; value.hotkeys = {{"model_token", "test-token"}}; return value; },
            [&] { ++moves; }, [&] { ++greetings; });
    };
    auto interaction = makeInteraction();
    auto click = [&](int x, int y) {
        interaction->PointerMoved(x, y);
        interaction->PointerButton(0, true); interaction->PointerButton(0, false);
    };

    // 不运行任何渲染帧；单击由独立 Qt 定时器完成且只派发一次。
    click(300, 200);
    CHECK(model.Taps() == 0);
    Wait(450);
    CHECK(model.Taps() == 1);
    CHECK(model.operations[0].value("mouse_x") == 300);
    CHECK(model.operations[0].value("model_token") == "test-token");
    Wait(400); CHECK(model.Taps() == 1);

    click(300, 200); click(300, 200);
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
    state.editing = true;
    interaction->ApplyLook(state, &output, .5f, .5f, 1); CHECK(!state.hasLook);

    // 已排队的旧超时不能抢先派发取消后新一轮的单击。
    CHECK(loop.Post({"interaction.tap_timeout", interaction.get(), interaction.get(), {}}));
    interaction->Cancel();
    click(300, 200);
    Wait(20); CHECK(model.Taps() == 3);
    interaction->Stop();
    const auto scale = service.Snapshot().transform.scale;
    click(300, 200); interaction->PointerScroll(1);
    CHECK(service.Snapshot().transform.scale == scale && greetings == 1);
    Wait(450); CHECK(model.Taps() == 3);
    interaction.reset();

    // 定时器仍活跃时销毁，且清除尚未派发的自身事件。
    interaction = makeInteraction();
    click(300, 200);
    CHECK(loop.Post({"interaction.tap_timeout", interaction.get(), interaction.get(), {}}));
    interaction.reset();
    Wait(450); CHECK(model.Taps() == 3);

    interaction = makeInteraction();
    click(300, 200);
    mode.Send(ModelWindowState::Event::Close);
    Wait(450); CHECK(model.Taps() == 3);
    mode.Send(ModelWindowState::Event::Released);
    interaction.reset();
    return 0;
}
