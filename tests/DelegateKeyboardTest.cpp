#include <GL/glew.h>
#include <EGL/egl.h>
#include "LAppDelegate.hpp"
#include "LAppDefine.hpp"
#include "ModelHotkeys.hpp"

#include <QDir>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
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

// Only the platform boundary is replaced. Delegate, input decisions, service,
// model discovery/loading, and the failure log all execute production code.
class HeadlessWindow : public DesktopWindow
{
public:
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
    bool Run(const std::function<bool()>& frame) override { return !frame(); }
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

// Inject a failure at the actual action endpoint, not a mirror trigger helper.
// Successful calls still execute the real ModelHotkeys implementation.
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
    CHECK(Write(modelDir + "test.vtube.json", R"({"Hotkeys":[{
        "HotkeyID":"binding-42","Action":"ToggleExpression","File":"test.exp3.json",
        "Triggers":{"Trigger1":"A"}},
        {"HotkeyID":"binding-42","Action":"ToggleExpression","File":"test.exp3.json"}]})"));
    CHECK(Write(modelDir + "test.exp3.json", R"({"Parameters":[{"Id":"ParamAngleX","Value":1
}]})"));
    CHECK(Write(directory.path() + "/config.json", "{}"));
    // Never connect to the user's IPC endpoint or modify their KWin scripts.
    qputenv("XDG_RUNTIME_DIR", directory.path().toUtf8());
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/deskwaifu-test-bus");
    LAppDefine::ModelsDir = directory.path().toStdString();
    auto& config = LAppConfig::GetInstance();
    config.sourcePath = (directory.path() + "/config.json").toStdString();
    config.values.modelOutput = "test";
    auto* app = LAppDelegate::GetInstance();
    CHECK(app->Initialize());
    CHECK(events);

    events->KeyboardKey(KEY_A, true);
    CHECK(triggerCalls == 0); // no grab
    app->GetWindow()->SetKeyboardGrab(true);
    std::ostringstream logs;
    auto* previous = std::cout.rdbuf(logs.rdbuf());
    events->KeyboardKey(KEY_A, true);
    std::cout.rdbuf(previous);
    CHECK(triggerCalls == 1);
    CHECK(logs.str().find("[Hotkeys] Trigger failed for binding binding-42: injected action failure") != std::string::npos);
    CHECK(triggered && !triggered->IsActive("binding-42"));
    events->KeyboardKey(KEY_A, true);
    CHECK(triggerCalls == 1); // repeat suppressed on the actual Delegate path
    events->KeyboardKey(KEY_A, false);
    failTrigger = false;
    logs.str("");
    previous = std::cout.rdbuf(logs.rdbuf());
    events->KeyboardKey(KEY_A, true);
    std::cout.rdbuf(previous);
    CHECK(triggerCalls == 2 && triggered->IsActive("binding-42"));
    CHECK(logs.str().find("Trigger failed") == std::string::npos);
    events->KeyboardKey(KEY_ESC, true);
    CHECK(!app->GetWindow()->KeyboardGrabbed());

    // Real IPC -> command adapter -> service: a changed model token must stop
    // stale actions, and unavailable duplicate IDs must not report active.
    const auto oldToken = triggered->Token();
    triggered->Reset();
    const auto token = triggered->Token();
    CHECK(token != oldToken && !triggered->IsActive("binding-42"));
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
        "{\"command\":\"get_hotkeys\"}\n{\"command\":\"quit\"}\n";
    CHECK(send(fd, requests.data(), requests.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(requests.size()));
    CHECK(app->Run()); // Poll handles all four requests; quit skips rendering and tears down.
    CHECK(triggerCalls == 3);
    QByteArray response;
    char buffer[4096];
    ssize_t count;
    while ((count = read(fd, buffer, sizeof(buffer))) > 0) response.append(buffer, count);
    close(fd);
    CHECK(count == 0);
    const auto lines = response.split('\n');
    CHECK(lines.size() == 5 && lines.back().isEmpty());
    const auto stale = QJsonDocument::fromJson(lines[0]).object();
    CHECK(!stale["ok"].toBool(true) && stale["error"].toString().contains("Model changed"));
    CHECK(QJsonDocument::fromJson(lines[1]).object()["ok"].toBool());
    const auto status = QJsonDocument::fromJson(lines[2]).object();
    CHECK(status["model_token"].toString().toStdString() == token);
    const auto bindings = status["hotkeys"].toArray();
    CHECK(bindings.size() == 2 && bindings[0].toObject()["active"].toBool());
    CHECK(!bindings[1].toObject()["active"].toBool(true));
    CHECK(!bindings[1].toObject()["reason"].toString().isEmpty());
    CHECK(QJsonDocument::fromJson(lines[3]).object()["ok"].toBool());
    return 0;
}
