#include <GL/glew.h>
#include <EGL/egl.h>
#include "LAppDelegate.hpp"
#include "LAppDefine.hpp"
#include "ModelHotkeys.hpp"
#include "ModelProcessorTestHelpers.hpp"

#include <QDir>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>
#include <cstring>
#include <iostream>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "Line " << __LINE__ << ": " << #condition << '\n'; return 1; \
} } while (false)

namespace {
IDesktopWindowEvents* events = nullptr;
bool failTrigger = true;
int triggerCalls = 0;
ModelHotkeys* triggered = nullptr;

// 只替换平台边界；Delegate、输入决策、服务、模型发现与加载以及失败日志
// 均执行正式代码。
class HeadlessWindow : public DesktopWindow
{
public:
    std::unique_ptr<ModelRenderTarget> RenderTarget(const std::string&) override { return CreateEglModelRenderTarget("auto"); }
    bool UploadFrame(const RenderFrame&) override { return true; }
    bool Initialize(int, int, IDesktopWindowEvents& receiver) override
    {
        events = &receiver;
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) return false;
        const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
        EGLConfig config;
        EGLint count = 0;
        if (!eglChooseConfig(display, attributes, &config, 1, &count) || count != 1 ||
            !eglBindAPI(EGL_OPENGL_API)) return false;
        const EGLint size[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        surface = eglCreatePbufferSurface(display, config, size);
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
        return surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT && MakeCurrent();
    }
    bool Run(const std::function<bool()>& frame) override
    {
        // 与平台契约一致：套接字就绪和帧使用同一个 Qt 派发器，
        // 但处理 IPC 不再要求先绘制一帧。
        bool quitRequested = false;
        QEventLoop loop;
        QTimer frames;
        QObject::connect(&frames, &QTimer::timeout, &loop, [&] {
            if (!frame()) { quitRequested = true; loop.quit(); }
        });
        frames.start(1);
        QTimer::singleShot(2000, &loop, &QEventLoop::quit);
        loop.exec();
        return quitRequested;
    }
    bool MakeCurrent() override { return eglMakeCurrent(display, surface, surface, context); }
    void Shutdown() override
    {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(display, context);
        eglDestroySurface(display, surface);
        eglTerminate(display);
    }
    bool Available() const override { return context != EGL_NO_CONTEXT; }
    int Width() const override { return 16; }
    int Height() const override { return 16; }
    unsigned int Framebuffer() const override { return 0; }
    bool SetInputRegion(const std::vector<ModelRegion>&) override { return true; }
    bool KeyboardGrabbed() const override { return grabbed; }
    ModelKeyboardGrabState::GrabResult SetKeyboardGrab(bool value) override
    {
        grabbed = value;
        return ModelKeyboardGrabState::GrabResult::Changed;
    }
    bool OutputNeedsRebind() const override { return false; }
    void ReconcileOutputs() override {}
    std::vector<OutputSnapshot> Outputs() const override { return {{1, "test", 0, 0, 16, 16}}; }
    OutputId CurrentOutput() const override { return 1; }
    bool SwitchOutput(OutputId id) override { return id == 1; }
private:
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    bool grabbed = false;
};

bool Write(const QString& path, const QByteArray& contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}
}

std::unique_ptr<DesktopWindow> CreateDesktopWindow()
{
    return std::unique_ptr<DesktopWindow>(new HeadlessWindow);
}

// 在真实动作执行入口注入失败，不使用复制逻辑的触发辅助函数。
// 成功调用仍执行真实 ModelHotkeys 实现。
extern "C" bool __real__ZN12ModelHotkeys7TriggerERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERS5_(
    ModelHotkeys*, const std::string&, std::string&);
extern "C" bool __wrap__ZN12ModelHotkeys7TriggerERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERS5_(
    ModelHotkeys* hotkeys, const std::string& id, std::string& error)
{
    ++triggerCalls;
    triggered = hotkeys;
    if (failTrigger) { error = "injected action failure"; return false; }
    return __real__ZN12ModelHotkeys7TriggerERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERS5_(hotkeys, id, error);
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    CHECK(directory.isValid());
    CHECK(QDir(directory.path()).mkdir("model"));
    const auto modelDir = directory.path() + "/model/";
    CHECK(QFile::link(QStringLiteral(SDK_RESOURCES "/Haru/Haru.moc3"), modelDir + "Haru.moc3"));
    CHECK(Write(modelDir + "test.model3.json",
        R"({"Version":3,"FileReferences":{"Moc":"Haru.moc3"}})"));
    CHECK(Write(modelDir + "z-other.model3.json", R"({"Version":3,"FileReferences":{"Moc":"Haru.moc3"}})"));
    CHECK(Write(modelDir + "test.vtube.json", R"({"Hotkeys":[{
        "HotkeyID":"binding-42","Action":"ToggleExpression","File":"test.exp3.json",
        "Triggers":{"Trigger1":"A"}},
        {"HotkeyID":"binding-42","Action":"ToggleExpression","File":"test.exp3.json"}]})"));
    CHECK(Write(modelDir + "test.exp3.json", R"({"Parameters":[{"Id":"ParamAngleX","Value":1
}]})"));
    CHECK(Write(directory.path() + "/config.json", "{}"));
    // 不连接用户的 IPC 端点，也不修改其 KWin 脚本。
    qputenv("XDG_RUNTIME_DIR", directory.path().toUtf8());
    qputenv("DESKWAIFU_RENDER_DEVICE", "auto");
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/deskwaifu-test-bus");
    LAppDefine::ModelsDir = directory.path().toStdString();
    auto& config = LAppConfig::GetInstance();
    config.sourcePath = (directory.path() + "/config.json").toStdString();
    config.values.modelOutput = "test";
    auto* app = LAppDelegate::GetInstance();
    CHECK(app->Initialize());
    CHECK(events);

    // 在不提供凭据、不联网的情况下测试正式双击路径。
    std::ostringstream speechLogs;
    auto* speechPrevious = std::cout.rdbuf(speechLogs.rdbuf());
    events->PointerMoved(8, 8);
    events->PointerButton(0, true);
    events->PointerButton(0, false);
    QCoreApplication::processEvents();
    const bool singleClickSilent = speechLogs.str().find("[Speech]") == std::string::npos;
    events->PointerButton(0, true);
    events->PointerButton(0, false);
    for (int i = 0; i < 4; ++i) QCoreApplication::processEvents();
    std::cout.rdbuf(speechPrevious);
    CHECK(singleClickSilent);
    CHECK(speechLogs.str().find("[Speech]") != std::string::npos);
    CHECK(speechLogs.str().find("MiniMax API Key") != std::string::npos);
    CHECK(app->GetWindow()->KeyboardGrabbed());
    app->GetWindow()->SetKeyboardGrab(false);

    events->KeyboardKey(KEY_A, true);
    CHECK(triggerCalls == 0); // 未捕获键盘。
    app->GetWindow()->SetKeyboardGrab(true);
    std::ostringstream logs;
    auto* previous = std::cout.rdbuf(logs.rdbuf());
    events->KeyboardKey(KEY_A, true);
    AwaitModel(app->Models());
    std::cout.rdbuf(previous);
    CHECK(triggerCalls == 1);
    CHECK(logs.str().find("injected action failure") != std::string::npos);
    CHECK(triggered && !app->Models().Metadata().hotkeys["hotkeys"].toArray()[0].toObject()["active"].toBool());
    events->KeyboardKey(KEY_A, true);
    CHECK(triggerCalls == 1); // 在真实 Delegate 路径上抑制重复按键。
    events->KeyboardKey(KEY_A, false);
    failTrigger = false;
    logs.str("");
    previous = std::cout.rdbuf(logs.rdbuf());
    events->KeyboardKey(KEY_A, true);
    AwaitModel(app->Models());
    std::cout.rdbuf(previous);
    CHECK(triggerCalls == 2 && app->Models().Metadata().hotkeys["hotkeys"].toArray()[0].toObject()["active"].toBool());
    CHECK(logs.str().find("Trigger failed") == std::string::npos);
    events->KeyboardKey(KEY_ESC, true);
    CHECK(!app->GetWindow()->KeyboardGrabbed());

    // 真实 IPC 到命令适配器再到服务：模型令牌变化必须阻止旧动作，
    // 不可用的重复 ID 不能报告为活动状态。
    const auto oldToken = app->Models().Metadata().hotkeys["model_token"].toString().toStdString();
    CHECK(AwaitModel(app->Models(), {{"command", "set_model"}, {"index", 1}})["ok"].toBool());
    CHECK(AwaitModel(app->Models(), {{"command", "set_model"}, {"index", 0}})["ok"].toBool());
    const auto token = app->Models().Metadata().hotkeys["model_token"].toString().toStdString();
    CHECK(token != oldToken && !app->Models().Metadata().hotkeys["hotkeys"].toArray()[0].toObject()["active"].toBool());
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(fd >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto path = directory.path().toStdString() + "/deskwaifu.sock";
    CHECK(path.size() < sizeof(address.sun_path));
    std::strcpy(address.sun_path, path.c_str());
    CHECK(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    const std::string requests = "{\"command\":\"trigger_hotkey\",\"id\":\"binding-42\",\"model_token\":\"" + oldToken + "\"}\n" +
        "{\"command\":\"trigger_hotkey\",\"id\":\"binding-42\",\"model_token\":\"" + token + "\"}\n" +
        "{\"command\":\"get_hotkeys\"}\n" +
        "{\"command\":\"get_render_device\"}\n" +
        "{\"command\":\"set_render_device\",\"device\":\"ffff:ff:1f.7\"}\n" +
        "{\"command\":\"set_render_device\",\"device\":\"auto\"}\n" +
        "{\"command\":\"quit\"}\n";
    CHECK(send(fd, requests.data(), requests.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(requests.size()));
    CHECK(app->Run()); // 套接字事件处理命令，quit 关闭窗口循环。
    CHECK(triggerCalls == 3);
    QByteArray response;
    char buffer[4096];
    ssize_t count;
    while ((count = read(fd, buffer, sizeof(buffer))) > 0) response.append(buffer, count);
    close(fd);
    CHECK(count == 0);
    const auto lines = response.split('\n');
    CHECK(lines.size() == 8 && lines.back().isEmpty());
    const auto stale = QJsonDocument::fromJson(lines[0]).object();
    CHECK(!stale["ok"].toBool(true) && stale["error"].toString().contains("Model changed"));
    CHECK(QJsonDocument::fromJson(lines[1]).object()["ok"].toBool());
    const auto status = QJsonDocument::fromJson(lines[2]).object();
    CHECK(status["model_token"].toString().toStdString() == token);
    const auto bindings = status["hotkeys"].toArray();
    CHECK(bindings.size() == 2 && bindings[0].toObject()["active"].toBool());
    CHECK(!bindings[1].toObject()["active"].toBool(true));
    CHECK(!bindings[1].toObject()["reason"].toString().isEmpty());
    const auto render = QJsonDocument::fromJson(lines[3]).object();
    CHECK(render["ok"].toBool() && render["device"] == "auto" && render["active_device"] == "auto");
    CHECK(render["devices"].isArray() && !render["restart_required"].toBool(true));
    const auto unavailable = QJsonDocument::fromJson(lines[4]).object();
    CHECK(!unavailable["ok"].toBool(true) && unavailable["error"].toString().contains("unavailable"));
    CHECK(unavailable["device"] == "auto");
    CHECK(QJsonDocument::fromJson(lines[5]).object()["ok"].toBool());
    CHECK(config.values.renderDevice == "auto");
    CHECK(QJsonDocument::fromJson(lines[6]).object()["ok"].toBool());
    return 0;
}
