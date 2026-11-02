#include <GL/glew.h>
#include <EGL/egl.h>
#include "TestApplication.hpp"
#include "processor/model/LAppDefine.hpp"
#include "processor/model/ModelHotkeys.hpp"
#include "ModelProcessorTestHelpers.hpp"
#include "DelegateTestWindow.hpp"

#include <QDir>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <iostream>
#include <sstream>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "Line " << __LINE__ << ": " << #condition << '\n'; return 1; \
} } while (false)

namespace {
IDesktopWindowEvents* events = nullptr;
bool failTrigger = true;
int triggerCalls = 0;
ModelHotkeys* triggered = nullptr;

bool Write(const QString& path, const QByteArray& contents)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

QJsonObject RequestDesktop(TestApplication& app, const QString& type, const QJsonObject& request)
{
    class Receiver final : public Processor
    {
    public:
        using Processor::Processor;
        bool done = false;
        QJsonObject reply;
        bool HandleEvent(const ProcessorEvent& event) override
        {
            if (event.receiver != this) return false;
            if (event.type == "model.result") reply = event.data.value("reply").toJsonObject();
            else if (event.type == "interaction.result")
                reply = QJsonDocument::fromJson(event.data.value("json").toString().toUtf8()).object();
            else return false;
            done = true; return true;
        }
    } receiver(ProcessorTestAccess::Loop(app.Models()));
    ProcessorTestAccess::Loop(app.Models()).Post({type, &receiver, &app.Interaction(),
        {{"json", QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact))}, {"connection", "keyboard-test"}}});
    QElapsedTimer timeout; timeout.start();
    while (!receiver.done && timeout.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    if (!receiver.done) qFatal("Timed out waiting for desktop command");
    return receiver.reply;
}
}

std::unique_ptr<DesktopWindow> CreateDesktopWindow()
{
    return std::make_unique<DelegateTestWindow>(&events);
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
    CHECK(argc == 1);
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
    auto* app = TestApplication::GetInstance();
    CHECK(app->Initialize());
    CHECK(events);

    // 正式双击路径抓取键盘并发布通用通知；可选插件集成由 platform 验证。
    int doubleClicks = 0;
    auto notifications = ProcessorTestAccess::Loop(app->Models()).CreateEndpoint("keyboard-test-notifications",
        [&](const EventDelivery&) { ++doubleClicks; });
    CHECK(notifications && notifications->Subscribe("interaction.double_click/v1"));
    events->PointerMoved(8, 8);
    events->PointerButton(0, true);
    events->PointerButton(0, false);
    for (int i = 0; i < 20; ++i) QCoreApplication::processEvents();
    CHECK(doubleClicks == 0);
    events->PointerButton(0, true);
    events->PointerButton(0, false);
    QElapsedTimer notificationWait; notificationWait.start();
    while (!doubleClicks && notificationWait.elapsed() < 5000) {
        QCoreApplication::processEvents(); QThread::msleep(1);
    }
    CHECK(doubleClicks == 1);
    CHECK(app->GetWindow()->KeyboardGrabbed());
    app->GetWindow()->SetKeyboardGrab(false);

    events->KeyboardKey(KEY_A, true);
    CHECK(triggerCalls == 0); // 未捕获键盘。
    app->GetWindow()->SetKeyboardGrab(true);
    std::ostringstream logs;
    auto* previous = std::cout.rdbuf(logs.rdbuf());
    events->KeyboardKey(KEY_A, true);
    AwaitModel(app->Models());
    // 模型操作完成不代表其结果事件已由交互处理器消费。
    QElapsedTimer resultWait; resultWait.start();
    while (logs.str().find("injected action failure") == std::string::npos && resultWait.elapsed() < 5000) {
        QCoreApplication::processEvents(); QThread::msleep(1);
    }
    std::cout.rdbuf(previous);
    CHECK(triggerCalls == 1);
    CHECK(logs.str().find("injected action failure") != std::string::npos);
    CHECK(triggered && !ModelMetadataOf(app->Models()).hotkeys["hotkeys"].toArray()[0].toObject()["active"].toBool());
    events->KeyboardKey(KEY_A, true);
    CHECK(triggerCalls == 1); // 在真实 Delegate 路径上抑制重复按键。
    events->KeyboardKey(KEY_A, false);
    failTrigger = false;
    logs.str("");
    previous = std::cout.rdbuf(logs.rdbuf());
    events->KeyboardKey(KEY_A, true);
    AwaitModel(app->Models());
    std::cout.rdbuf(previous);
    CHECK(triggerCalls == 2 && ModelMetadataOf(app->Models()).hotkeys["hotkeys"].toArray()[0].toObject()["active"].toBool());
    CHECK(logs.str().find("Trigger failed") == std::string::npos);
    events->KeyboardKey(KEY_ESC, true);
    CHECK(!app->GetWindow()->KeyboardGrabbed());

    // 本域真实交互/模型事件路径：模型令牌变化必须阻止旧动作，
    // 不可用的重复 ID 不能报告为活动状态。IPC/插件接线不属于此单测。
    const auto oldToken = ModelMetadataOf(app->Models()).hotkeys["model_token"].toString().toStdString();
    CHECK(AwaitModel(app->Models(), {{"command", "set_model"}, {"index", 1}})["ok"].toBool());
    CHECK(AwaitModel(app->Models(), {{"command", "set_model"}, {"index", 0}})["ok"].toBool());
    const auto token = ModelMetadataOf(app->Models()).hotkeys["model_token"].toString().toStdString();
    CHECK(token != oldToken && !ModelMetadataOf(app->Models()).hotkeys["hotkeys"].toArray()[0].toObject()["active"].toBool());
    const auto stale = RequestDesktop(*app, "interaction.model", {{"command", "trigger_hotkey"},
        {"id", "binding-42"}, {"model_token", QString::fromStdString(oldToken)}});
    CHECK(!stale["ok"].toBool(true) && stale["error"].toString().contains("Model changed"));
    CHECK(RequestDesktop(*app, "interaction.model", {{"command", "trigger_hotkey"},
        {"id", "binding-42"}, {"model_token", QString::fromStdString(token)}})["ok"].toBool());
    CHECK(triggerCalls == 3);
    const auto status = RequestDesktop(*app, "interaction.model", {{"command", "get_hotkeys"}});
    CHECK(status["model_token"].toString().toStdString() == token);
    const auto bindings = status["hotkeys"].toArray();
    CHECK(bindings.size() == 2 && bindings[0].toObject()["active"].toBool());
    CHECK(!bindings[1].toObject()["active"].toBool(true));
    CHECK(!bindings[1].toObject()["reason"].toString().isEmpty());
    const auto render = RequestDesktop(*app, "interaction.command", {{"command", "get_render_device"}});
    CHECK(render["ok"].toBool() && render["device"] == "auto" && render["active_device"] == "auto");
    CHECK(render["devices"].isArray() && !render["restart_required"].toBool(true));
    const auto unavailable = RequestDesktop(*app, "interaction.command", {{"command", "set_render_device"}, {"device", "ffff:ff:1f.7"}});
    CHECK(!unavailable["ok"].toBool(true) && unavailable["error"].toString().contains("unavailable"));
    CHECK(unavailable["device"] == "auto");
    CHECK(RequestDesktop(*app, "interaction.command", {{"command", "set_render_device"}, {"device", "auto"}})["ok"].toBool());
    LAppConfig savedConfig;
    CHECK(savedConfig.LoadFromFile(config.sourcePath) && savedConfig.values.renderDevice == "auto");
    CHECK(RequestDesktop(*app, "interaction.command", {{"command", "quit"}})["ok"].toBool());
    CHECK(ProcessorTestAccess::Loop(app->Models()).Post({"application.quit"}));
    CHECK(app->Run()); // 真正的呈现循环观察关闭，再执行 GPU/窗口有序收尾。
    return 0;
}
