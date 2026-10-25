#include <GL/glew.h>
#include <EGL/egl.h>
#include "TestApplication.hpp"
#include "processor/model/LAppView.hpp"
#include "processor/model/LAppModel.hpp"
#include "processor/model/LAppLive2DManager.hpp"
#include "processor/model/LAppDefine.hpp"
#include "processor/model/LAppTextureManager.hpp"
#include "LAppAllocator_Common.hpp"
#include "ModelProcessorTestHelpers.hpp"
#include "DispatchedInput.hpp"
#include <Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp>
#include <Id/CubismIdManager.hpp>
#include "TestTemporaryDirectory.hpp"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "Line " << __LINE__ << ": " << #condition << '\n'; return 1; \
} } while (false)

namespace {
struct Hit { std::string area; float x, y; bool result; };
std::vector<Hit> hits;
int expressionCalls = 0;

class Window : public DesktopWindow
{
public:
    std::unique_ptr<ModelRenderTarget> RenderTarget(const std::string&) override { return CreateEglModelRenderTarget("auto"); }
    bool UploadFrame(const RenderFrame&) override { return true; }
    IDesktopWindowEvents* events = nullptr;
    DispatchedInput input;
    std::vector<OutputSnapshot> outputs{{1, "left", 0, 0, 800, 600},
        {2, "right", 800, 0, 1000, 800}};
    OutputId current = 1;
    bool failSwitch = false, grabbed = false;
    bool needsRebind = false;
    std::function<void()> duringSwitch;
    int switches = 0;
    std::function<bool(const std::function<bool()>&)> script;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    bool Initialize(int, int, IDesktopWindowEvents& sink) override
    {
        input.Set(sink); events = &input;
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) return false;
        const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
        EGLConfig config; EGLint count = 0;
        if (!eglChooseConfig(display, attributes, &config, 1, &count) || count != 1 || !eglBindAPI(EGL_OPENGL_API)) return false;
        const EGLint size[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        surface = eglCreatePbufferSurface(display, config, size);
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
        return surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT && MakeCurrent();
    }
    bool Run(const std::function<bool()>& frame) override { return script && script(frame); }
    bool MakeCurrent() override { return eglMakeCurrent(display, surface, surface, context); }
    void Shutdown() override
    {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context); eglDestroySurface(display, surface); eglTerminate(display);
    }
    bool Available() const override { return true; }
    int Width() const override { return current == 1 ? outputs[0].width : outputs[1].width; }
    int Height() const override { return current == 1 ? outputs[0].height : outputs[1].height; }
    unsigned Framebuffer() const override { return 0; }
    bool SetInputRegion(const std::vector<ModelRegion>&) override { return true; }
    bool KeyboardGrabbed() const override { return grabbed; }
    ModelKeyboardGrabState::GrabResult SetKeyboardGrab(bool value) override
    { grabbed = value; return ModelKeyboardGrabState::GrabResult::Changed; }
    bool OutputNeedsRebind() const override { return needsRebind; }
    void ReconcileOutputs() override { if (needsRebind) SwitchOutput(current); }
    std::vector<OutputSnapshot> Outputs() const override { return outputs; }
    OutputId CurrentOutput() const override { return current; }
    bool SwitchOutput(OutputId id) override
    {
        events->OutputSwitchStarted();
        if (duringSwitch) duringSwitch();
        if (!failSwitch) { current = id; ++switches; }
        needsRebind = false;
        events->OutputSwitchFinished();
        return !failSwitch;
    }
};
Window* window = nullptr;
bool near(float a, float b) { return std::abs(a - b) < .0001f; }

void click(int x, int y)
{
    window->events->PointerMoved(x, y);
    window->events->PointerButton(0, true);
    window->events->PointerButton(0, false);
}

int releaseTest(TestApplication& app)
{
    CHECK(AwaitModel(app.Models(), {{"command", "tap_model"}, {"mouse_x", 150}, {"mouse_y", 120}})["ok"].toBool());
    CHECK(!hits.empty());
    CHECK(near(hits[0].x, -250.f / 300) && near(hits[0].y, 180.f / 300));
    hits.clear();
    CHECK(AwaitModel(app.Models(), {{"command", "tap_model"}, {"mouse_x", 700}, {"mouse_y", 500}})["ok"].toBool());
    CHECK(!hits.empty());
    CHECK(near(hits[0].x, 1) && near(hits[0].y, -200.f / 300));
    hits.clear();
    // 真实 Delegate 的延迟单击只派发一次。远处的第二次释放会立即派发它，
    // 无需等待帧定时器。
    QElapsedTimer ready; ready.start();
    while (!ModelFrameOf(app.Models()) && ready.elapsed() < 5000) {
        QCoreApplication::processEvents(); QThread::msleep(1);
    }
    CHECK(ModelFrameOf(app.Models()));
    click(300, 200);
    CHECK(hits.empty());
    const int expressionBefore = expressionCalls;
    click(500, 400);
    AwaitModel(app.Models());
    CHECK(expressionCalls == expressionBefore + 1);
    // 将第二次点击补成双击，其待处理单击必须被取消。
    hits.clear();
    click(500, 400);
    CHECK(hits.empty() && window->grabbed);
    window->events->KeyboardKey(1, true); // Escape 通过真实路径释放键盘捕获。
    CHECK(!window->grabbed);
    window->events->KeyboardChanged();
    const auto before = app.Service().Snapshot().transform;
    window->events->PointerScroll(1);
    CHECK(app.Service().Snapshot().transform.scale > before.scale);
    CHECK(app.Service().SetTransform({2, .1f, -.2f}).ok);
    hits.clear();
    click(400, 300);
    CHECK(hits.empty());
    // 不绘制任何帧，Qt 定时器仍须派发延迟单击。
    QEventLoop wait;
    QTimer::singleShot(ModelDoubleClick::Delay + 20, &wait, &QEventLoop::quit);
    wait.exec();
    AwaitModel(app.Models());
    CHECK(expressionCalls == expressionBefore + 2);
    return 0;
}

int transformTest(TestApplication& app)
{
    // 模型命中测试坐标中的可见点为 (0.2, 0.3)。
    // 独立于渲染实现约定推导其像素位置。
    for (const auto size : {std::pair<int, int>{800, 600}, {600, 800}}) {
        window->outputs[0].width = size.first; window->outputs[0].height = size.second;
        for (const auto t : {ModelTransform{1, 0, 0}, {1, .2f, -.1f}, {2, 0, 0}, {2, .1f, -.2f}}) {
            CHECK(app.Service().SetTransform(t).ok);
            const float x = size.first * .5f + (.2f + t.x) * t.scale * size.second * .5f;
            const float y = size.second * .5f - (.3f + t.y) * t.scale * size.second * .5f;
            hits.clear();
            CHECK(AwaitModel(app.Models(), {{"command", "tap_model"}, {"mouse_x", x}, {"mouse_y", y},
                {"width", size.first}, {"height", size.second}, {"zoom", t.scale}, {"x", t.x}, {"y", t.y}})["ok"].toBool());
            CHECK(!hits.empty() && near(hits[0].x, .2f) && near(hits[0].y, .3f));
            CHECK(hits[0].area == "Head" && hits[0].result); // 实际的回退命中测试。
        }
    }

    // 使用真实渲染器的最终 MVP，不在测试中重写投影公式。
    // 覆盖已配置的 HitAreas、回退判定及竖屏适配。
    CHECK(app.Models().Stop());
    // 底层几何检查使用独立且按顺序执行的 Cubism 生命周期。
    // 上面的应用级检查只通过操作事件访问模型。
    CHECK(eglInitialize(window->display, nullptr, nullptr));
    CHECK(window->MakeCurrent());
    LAppAllocator_Common allocator;
    Csm::CubismFramework::Option option{};
    CHECK(Csm::CubismFramework::StartUp(&allocator, &option));
    Csm::CubismFramework::Initialize();
    struct Cleanup { ~Cleanup() { Csm::CubismFramework::Dispose(); Csm::CubismFramework::CleanUp(); } } cleanup;
    LAppTextureManager textures;
    LAppLive2DManager manager(textures, 800, 600,
        {LAppDefine::ModelsDir, {}, "", -1});
    class View : public LAppView {
    public:
        using LAppView::LAppView;
        void ChangeView() { _viewMatrix->Scale(1.3f, 1.3f); _viewMatrix->Translate(.1f, -.2f); }
    } view(manager, textures);
    for (int index = 0; index < manager.GetModelDirSize(); ++index) {
        CHECK(manager.ChangeScene(index));
        auto* model = manager.GetModel(0);
        if (index == 2) CHECK(model->GetModel()->GetCanvasWidth() > 1.f);
        for (const auto size : {std::pair<int, int>{800, 600}, {600, 800}}) {
            view.Initialize(size.first, size.second);
            view.ChangeView();
            manager.SetRenderTargetSize(size.first, size.second);
            for (const auto t : {ModelTransform{1, 0, 0}, {1, .2f, -.1f}, {2, 0, 0}, {2, .1f, -.2f}}) {
                view.Render(t, {});
                auto mvp = model->GetRenderer<Csm::Rendering::CubismRenderer_OpenGLES2>()->GetMvpMatrix();
                for (const bool head : {true, false}) {
                    float x = .2f, y = head ? .3f : -.3f;
                    if (index == 1) {
                        const auto id = Csm::CubismFramework::GetIdManager()->GetId(head ? "HitArea" : "HitArea2");
                        const int drawable = model->GetModel()->GetDrawableIndex(id);
                        CHECK(drawable >= 0);
                        const auto* vertices = model->GetModel()->GetDrawableVertices(drawable);
                        const int count = model->GetModel()->GetDrawableVertexCount(drawable);
                        CHECK(count > 0);
                        float left = vertices[0], right = left, bottom = vertices[1], top = bottom;
                        for (int i = 1; i < count; ++i) {
                            left = std::min(left, vertices[i * 2]); right = std::max(right, vertices[i * 2]);
                            bottom = std::min(bottom, vertices[i * 2 + 1]); top = std::max(top, vertices[i * 2 + 1]);
                        }
                        x = model->GetModelMatrix()->TransformX((left + right) * .5f);
                        y = model->GetModelMatrix()->TransformY((bottom + top) * .5f);
                    }
                    const float px = (mvp.TransformX(model->GetModelMatrix()->InvertTransformX(x)) + 1) * size.first * .5f;
                    const float py = (1 - mvp.TransformY(model->GetModelMatrix()->InvertTransformY(y))) * size.second * .5f;
                    hits.clear();
                    view.OnTouchesBegan(0, 0);
                    view.OnTouchesEnded(px, py, t);
                    CHECK(hits.size() == 1 && near(hits[0].x, x) && near(hits[0].y, y));
                    CHECK(hits[0].area == (head ? "Head" : "Body") && hits[0].result);
                    const auto snapshot = manager.HitSnapshot(size.first, size.second, t);
                    const auto hit = snapshot.Test({px, py, size.first, size.second, snapshot.modelToken});
                    CHECK(hit.available);
                    CHECK(std::find(hit.parts.begin(), hit.parts.end(), head ? "Head" : "Body") != hit.parts.end());
                }
            }
        }
    }
    return 0;
}

int releaseWithCursor(int x, int y)
{
    const std::string directory = std::string(getenv("XDG_RUNTIME_DIR")) + "/hypr";
    CHECK(mkdir(directory.c_str(), 0700) == 0);
    CHECK(mkdir((directory + "/pointer-test").c_str(), 0700) == 0);
    const auto path = directory + "/pointer-test/.socket.sock";
    const int server = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(server >= 0);
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    CHECK(path.size() < sizeof(address.sun_path));
    std::strcpy(address.sun_path, path.c_str());
    CHECK(bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    CHECK(listen(server, 1) == 0);
    bool queried = false;
    std::thread compositor([&] {
        pollfd ready{server, POLLIN, 0};
        if (poll(&ready, 1, 1000) <= 0) return;
        const int client = accept(server, nullptr, nullptr);
        if (client < 0) return;
        char request[32]{};
        const auto n = read(client, request, sizeof(request));
        queried = n == 12 && std::string(request, n) == "-j/cursorpos";
        const std::string response = "{\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) + "}";
        send(client, response.c_str(), response.size(), MSG_NOSIGNAL);
        close(client);
    });
    window->events->PointerButton(0, false);
    compositor.join();
    close(server); unlink(path.c_str());
    rmdir((directory + "/pointer-test").c_str()); rmdir(directory.c_str());
    CHECK(queried);
    return 0;
}

int dragTest(TestApplication& app)
{
    const int initialSwitches = window->switches;
    auto drag = [&] {
        window->events->PointerMoved(400, 300);
        window->events->PointerButton(0, true);
        window->events->PointerMoved(1100, 350);
    };
    // 缺少全局光标能力时，本地裁剪事务仍应可用。
    drag();
    CHECK(app.Settings().LocalScreenEditing());
    window->events->PointerButton(0, false);
    CHECK(window->current == 1 && app.Settings().LocalScreenEditing());
    CHECK(app.Settings().Cancel("local-screen").ok);

    // 同一屏幕上的有效光标不能触发重绑定或重置裁剪。
    drag();
    const auto region = app.Settings().Display().region;
    CHECK(releaseWithCursor(700, 350) == 0);
    CHECK(window->switches == initialSwitches && app.Settings().Display().region.width == region.width);
    CHECK(app.Settings().Cancel("local-screen").ok);

    // 重绑定失败应完整保留草稿，不触发保存。
    window->failSwitch = true;
    drag();
    const auto preview = app.Settings().Display().transform;
    CHECK(releaseWithCursor(1100, 350) == 0);
    CHECK(window->current == 1 && app.Settings().LocalScreenEditing());
    CHECK(app.Settings().Display().transform.x == preview.x);
    CHECK(app.Settings().Cancel("local-screen").ok);
    window->failSwitch = false;

    // 显式控制面板屏幕编辑仍使用普通本地裁剪手势。
    CHECK(app.Settings().Begin("panel", true, 800, 600).ok);
    drag();
    window->events->PointerButton(0, false);
    CHECK(window->current == 1 && app.Settings().OwnedBy("panel"));
    CHECK(app.Settings().Cancel("panel").ok);

    // 调整裁剪边缘也不属于跨屏移动模型。
    CHECK(app.Settings().BeginLocalScreenEdit(800, 600).ok);
    window->events->PointerMoved(0, 300);
    window->events->PointerButton(0, true);
    window->events->PointerMoved(50, 300);
    window->events->PointerButton(0, false);
    CHECK(window->current == 1 && app.Settings().Display().region.width == 750);
    CHECK(app.Settings().Cancel("local-screen").ok);

    drag();
    CHECK(app.Settings().LocalScreenEditing());
    CHECK(releaseWithCursor(1100, 350) == 0);
    CHECK(window->current == 2);
    CHECK(hits.empty() && !window->grabbed);
    CHECK(app.Settings().LocalScreenEditing());
    const auto destination = app.Settings().Display();
    CHECK(destination.region.x == 0 && destination.region.y == 0);
    CHECK(destination.region.width == 1000 && destination.region.height == 800);
    CHECK(app.GetWindowWidth() == 1000 && app.GetWindowHeight() == 800);
    CHECK(near(destination.transform.scale, .75f));
    window->events->PointerButton(0, false); // 没有捕获的拖动，不应再次切换。
    CHECK(window->switches == initialSwitches + 1);
    return 0;
}

int stateTest(TestApplication& app)
{
    using State = ModelWindowState::State;
    CHECK(app.WindowState() == State::NormalIdle);
    click(300, 200); // 开始编辑时必须丢弃待处理的模型点击。
    CHECK(app.Settings().Begin("panel", false, 800, 600).ok);
    CHECK(app.WindowState() == State::PanelEditing);
    const auto transform = app.Service().Snapshot().transform;
    click(400, 300);
    window->events->PointerScroll(1);
    CHECK(app.Service().Snapshot().transform.scale == transform.scale);
    CHECK(app.Settings().Cancel("panel").ok);
    CHECK(app.WindowState() == State::NormalIdle);
    // 同位置点击不能与之前的点击组成双击。
    click(300, 200);
    CHECK(!window->grabbed && hits.empty());
    CHECK(app.Settings().Begin("panel", true, 800, 600).ok);
    CHECK(app.WindowState() == State::EditIdle);
    CHECK(!app.Settings().Apply("wrong-owner").ok);
    CHECK(app.WindowState() == State::EditIdle);
    window->events->PointerMoved(400, 300);
    window->events->PointerButton(0, true);
    CHECK(app.WindowState() == State::DraggingModel);
    window->events->PointerMoved(420, 320);
    CHECK(app.Settings().Display().transform.x != transform.x);
    window->events->PointerButton(0, false);
    CHECK(app.WindowState() == State::EditIdle);
    const auto beforeMove = app.Settings().Display();
    window->events->PointerMoved(400, 4);
    window->events->PointerButton(0, true);
    CHECK(app.WindowState() == State::MovingCrop);
    window->events->PointerMoved(410, 14);
    CHECK(app.Settings().Display().region.x == beforeMove.region.x + 10);
    CHECK(app.Settings().Display().transform.x != beforeMove.transform.x);
    window->events->PointerButton(0, false);
    CHECK(app.WindowState() == State::EditIdle);
    window->events->PointerMoved(10, 300);
    window->events->PointerButton(0, true);
    CHECK(app.WindowState() == State::ResizingCrop);
    const auto beforeResize = app.Settings().Display().transform;
    window->events->PointerMoved(60, 300);
    CHECK(app.Settings().Display().region.width == 750);
    CHECK(app.Settings().Display().transform.x == beforeResize.x);
    // 面板取消操作应立即结束之前捕获的手势。
    CHECK(app.Settings().Cancel("panel").ok);
    CHECK(app.WindowState() == State::NormalIdle);
    CHECK(app.Settings().Begin("next-panel", true, 800, 600).ok);
    const auto fresh = app.Settings().Display();
    window->events->PointerMoved(100, 300);
    window->events->PointerButton(0, false);
    CHECK(app.Settings().Display().region.width == fresh.region.width);
    CHECK(app.WindowState() == State::EditIdle);
    CHECK(app.Settings().Apply("next-panel").ok);
    CHECK(app.WindowState() == State::NormalIdle);
    // 隐藏和显示不能残留捕获的按下状态或待处理双击。
    window->events->PointerMoved(400, 300);
    window->events->PointerButton(0, true);
    CHECK(app.Service().SetVisibility(false).ok);
    CHECK(app.WindowState() == State::NormalIdle);
    CHECK(app.Service().SetVisibility(true).ok);
    window->events->PointerButton(0, false);
    CHECK(!window->grabbed && hits.empty());
    // 切换会取消当前手势，但保留主动编辑的草稿。
    window->events->PointerMoved(400, 300);
    window->events->PointerButton(0, true);
    CHECK(app.WindowState() == State::Pressed);
    window->events->PointerMoved(450, 350);
    CHECK(app.WindowState() == State::DraggingModel);
    const auto draft = app.Settings().Display();
    bool blocked = false;
    window->duringSwitch = [&] {
        blocked = app.WindowState() == State::SwitchingOutput;
        window->events->PointerMoved(700, 500);
        window->events->PointerButton(0, true);
        window->events->PointerScroll(1);
    };
    CHECK(window->SwitchOutput(1));
    CHECK(blocked && app.WindowState() == State::EditIdle);
    CHECK(app.Settings().Display().transform.x == draft.transform.x);
    CHECK(app.Settings().Display().transform.scale == draft.transform.scale);
    window->events->PointerButton(0, false);
    CHECK(app.Settings().LocalScreenEditing());
    // 与主动切换不同，热插拔也会取消事务。
    window->needsRebind = true;
    window->ReconcileOutputs();
    CHECK(!app.Settings().Editing() && app.WindowState() == State::NormalIdle);
    window->duringSwitch = {};
    // 嵌套等待输出设备时触发的关闭，不能被 OutputReady 撤销。
    window->duringSwitch = [&] { app.AppEnd(); };
    CHECK(window->SwitchOutput(1));
    CHECK(app.GetIsEnd() && app.WindowState() == State::Closing);
    const auto closing = app.Service().Snapshot().transform;
    click(400, 300);
    window->events->PointerScroll(1);
    CHECK(app.Service().Snapshot().transform.scale == closing.scale);
    CHECK(hits.empty());
    return 0;
}
}

// 观察真实 Manager 和命中测试路径发出的诊断信息，
// 不用测试专用实现替换命中决策或动作。
extern "C" void __real__ZN9LAppModel19SetRandomExpressionEv(LAppModel* model);
extern "C" void __wrap__ZN9LAppModel19SetRandomExpressionEv(LAppModel* model)
{
    ++expressionCalls;
    __real__ZN9LAppModel19SetRandomExpressionEv(model);
}

extern "C" void __wrap__ZN7LAppPal10PrintLogLnEPKcz(const char* format, ...)
{
    va_list args; va_start(args, format);
    if (std::strcmp(format, "[APP]tap point: {x:%.2f y:%.2f}") == 0) {
        const float x = va_arg(args, double), y = va_arg(args, double);
        hits.push_back({{}, x, y, false});
    } else if (std::strcmp(format, "[APP]hit area: [%s]") == 0 && !hits.empty()) {
        hits.back().area = va_arg(args, const char*);
        hits.back().result = true;
    }
    va_end(args);
    va_start(args, format); std::vprintf(format, args); std::puts(""); va_end(args);
}
std::unique_ptr<DesktopWindow> CreateDesktopWindow()
{
    std::unique_ptr<Window> result(new Window);
    window = result.get();
    return std::move(result);
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    CHECK(argc == 2);
    const auto directory = MakeTestTemporaryDirectory("deskwaifu-pointer");
    CHECK(!directory.empty());
    setenv("XDG_RUNTIME_DIR", directory.c_str(), 1);
    setenv("HYPRLAND_INSTANCE_SIGNATURE", "pointer-test", 1);
    unsetenv("DBUS_SESSION_BUS_ADDRESS"); // 不在用户会话中加载脚本。
    const auto models = directory + "/models";
    CHECK(mkdir(models.c_str(), 0700) == 0);
    const auto modelDirectory = models + "/pointer";
    CHECK(mkdir(modelDirectory.c_str(), 0700) == 0);
    const auto model = modelDirectory + "/pointer.model3.json";
    const auto moc = modelDirectory + "/pointer.moc3";
    const auto configured = modelDirectory + "/z-hit.model3.json";
    const auto wide = modelDirectory + "/zz-wide.model3.json";
    const auto wideMoc = modelDirectory + "/wide.moc3";
    CHECK(symlink(SDK_RESOURCES "/Haru/Haru.moc3", moc.c_str()) == 0);
    CHECK(symlink(SDK_RESOURCES "/Rice/Rice.moc3", wideMoc.c_str()) == 0);
    {
        std::ofstream file(model);
        file << "{\"Version\":3,\"FileReferences\":{\"Moc\":\"pointer.moc3\"}}";
        CHECK(file.good());
    }
    {
        std::ofstream file(configured);
        file << "{\"Version\":3,\"FileReferences\":{\"Moc\":\"pointer.moc3\"},"
            "\"HitAreas\":[{\"Id\":\"HitArea\",\"Name\":\"Head\"},{\"Id\":\"HitArea2\",\"Name\":\"Body\"}]}";
        CHECK(file.good());
    }
    { std::ofstream file(wide); file << "{\"Version\":3,\"FileReferences\":{\"Moc\":\"wide.moc3\"}}"; }
    const auto configPath = directory + "/config.json";
    { std::ofstream file(configPath); file << "{\"model_scale\":1,\"model_output\":\"left\"}"; }
    auto& config = LAppConfig::GetInstance();
    CHECK(config.LoadFromFile(configPath));
    LAppDefine::ModelsDir = models;
    auto* app = TestApplication::GetInstance();
    if (!app->Initialize()) return window->context == EGL_NO_CONTEXT ? 77 : 1;
    ConfigStore configStore(configPath);
    ConfigStore::Snapshot beforeDrag;
    std::string configError;
    CHECK(configStore.Read(beforeDrag, configError));
    const std::string scenario = argv[1];
    const int result = scenario == "release" ? releaseTest(*app) :
        scenario == "transform" ? transformTest(*app) : scenario == "state" ? stateTest(*app) : dragTest(*app);
    if (result == 0 && scenario == "drag") {
        ConfigStore::Snapshot disk;
        CHECK(configStore.Read(disk, configError) && disk.document == beforeDrag.document);
    }
    if (result == 0 && scenario != "transform" && scenario != "state") {
        window->script = [&](const std::function<bool()>& frame) {
            if (!frame()) return false;
            // 主动跨输出设备重绑定后的第一个真实帧，
            // 不能进入热插拔或尺寸变化的取消路径。
            if (scenario == "drag") {
                if (window->current != 2 || !app->Settings().LocalScreenEditing() ||
                    app->Settings().Display().region.width != 1000) return false;
                const auto draft = app->Settings().Display();
                ConfigStore::Snapshot disk;
                if (!configStore.Read(disk, configError) || disk.document != beforeDrag.document) return false;
                // 将真实编辑器时间推进到 0.5 秒自动保存防抖期之后，
                // 不为指针回归测试添加实际等待。
                const double now = std::chrono::duration<double>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                app->Settings().Tick(now + 1);
                if (!frame() || !app->Settings().LocalScreenEditing() ||
                    !configStore.Read(disk, configError) || disk.document != beforeDrag.document) return false;
                if (!app->Settings().Apply("local-screen").ok || app->Settings().Editing() ||
                    !configStore.Read(disk, configError) || disk.document == beforeDrag.document) return false;
                TransformSettings saved;
                return TransformEditor::Decode(disk, saved, configError) && saved.cropped == draft.cropped &&
                    saved.region.x == draft.region.x && saved.region.y == draft.region.y &&
                    saved.region.width == draft.region.width && saved.region.height == draft.region.height &&
                    near(saved.transform.scale, draft.transform.scale) &&
                    near(saved.transform.x, draft.transform.x) && near(saved.transform.y, draft.transform.y) &&
                    disk.values.at("model_output") == beforeDrag.values.at("model_output");
            }
            AwaitModel(app->Models());
            return expressionCalls >= 2;
        };
        CHECK(app->Run()); // 同时执行真实帧和 GPU 清理路径。
    } else {
        CHECK(app->Release());
        TestApplication::ReleaseInstance();
    }
    CHECK(unlink(model.c_str()) == 0 && unlink(moc.c_str()) == 0 && unlink(configured.c_str()) == 0 &&
        unlink(wide.c_str()) == 0 && unlink(wideMoc.c_str()) == 0 &&
        rmdir(modelDirectory.c_str()) == 0 && rmdir(models.c_str()) == 0);
    CHECK(unlink(configPath.c_str()) == 0);
    // IPC 所有权锁刻意保留在运行时目录中。
    unlink((directory + "/deskwaifu.sock.lock").c_str());
    CHECK(rmdir(directory.c_str()) == 0);
    return result;
}
