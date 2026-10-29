#include "plugin/PluginManager.hpp"
#include "processor/Processor.hpp"
#include "contracts/ConfigStore.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Wait(const std::function<bool()>& done, const char* message)
{
    QElapsedTimer timer; timer.start();
    while (!done() && timer.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    Check(done(), message);
}
void Write(const QString& path, const QByteArray& contents)
{
    QFile file(path);
    Check(file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size(), "write private fixture");
}
void Install(const QString& source, const QString& root, const QString& name)
{
    Check(QDir().mkpath(root + '/' + name), "private plugin directory");
    Check(QFile::copy(source, root + '/' + name + '/' + name + ".so"), "install actual plugin module");
}
QJsonObject Request(QLocalSocket& socket, const QJsonObject& request)
{
    const auto json = QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    Check(socket.write(json) == json.size() && socket.flush(), "send public command");
    Wait([&] { return socket.canReadLine(); }, "public command completion timed out");
    return QJsonDocument::fromJson(socket.readLine()).object();
}
class QuitObserver final : public Processor
{
public:
    using Processor::Processor;
    int requests = 0;
    bool HandleEvent(const ProcessorEvent& event) override
    {
        if (event.type == "application.quit") ++requests;
        return false;
    }
};
}

int main(int argc, char** argv)
try {
    QCoreApplication app(argc, argv);
    Check(argc == 4, "expected desktop window fixture, IPC and speech modules");
    QTemporaryDir directory;
    Check(directory.isValid(), "private delegate installation");
    const int runtimeFd = open(QFile::encodeName(directory.path()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    Check(runtimeFd >= 0, "private runtime directory descriptor");
    const auto closeRuntime = qScopeGuard([runtimeFd] { close(runtimeFd); });
    const auto runtime = QStringLiteral("/proc/self/fd/%1").arg(runtimeFd);
    const auto configHome = directory.path() + "/config";
    const auto configRoot = configHome + "/deskwaifu";
    const auto modelRoot = configRoot + "/models/model";
    Check(QDir().mkpath(modelRoot), "private model directory");
    Check(QFile::link(QStringLiteral(SDK_RESOURCES "/Haru/Haru.moc3"), modelRoot + "/Haru.moc3"), "model resource fixture");
    Write(modelRoot + "/test.model3.json", R"({"Version":3,"FileReferences":{"Moc":"Haru.moc3"}})");
    Write(modelRoot + "/z-other.model3.json", R"({"Version":3,"FileReferences":{"Moc":"Haru.moc3"}})");
    Write(modelRoot + "/test.vtube.json", R"({"Hotkeys":[
        {"HotkeyID":"binding-42","Action":"ToggleExpression","File":"test.exp3.json","Triggers":{"Trigger1":"A"}},
        {"HotkeyID":"binding-42","Action":"ToggleExpression","File":"test.exp3.json"}]})");
    Write(modelRoot + "/test.exp3.json", R"({"Parameters":[{"Id":"ParamAngleX","Value":1}]})");
    Write(configRoot + "/config.json", R"({"model_output":"test","render_device":"auto"})");
    qputenv("XDG_CONFIG_HOME", configHome.toUtf8());
    qputenv("XDG_RUNTIME_DIR", runtime.toUtf8());
    qputenv("DESKWAIFU_RENDER_DEVICE", "auto");
    qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/deskwaifu-delegate-test-bus");
    const auto pluginRoot = directory.path() + "/plugins";
    Install(app.arguments()[1], pluginRoot, "desktop");
    Install(app.arguments()[2], pluginRoot, "ipc");
    Install(app.arguments()[3], pluginRoot, "speech");
    ProcessorEventLoop loop;
    QuitObserver quit(loop);
    PluginManager plugins(loop, pluginRoot, configRoot);
    QString error;
    Check(plugins.Start(&error), qPrintable(error));
    Check(plugins.LoadedPlugins() == QStringList{"desktop", "ipc", "speech"}, "all three actual plugins must load");
    QLocalSocket socket;
    socket.connectToServer(runtime + "/deskwaifu.sock");
    Check(socket.waitForConnected(1000), "connect private actual IPC transport");
    const auto initial = Request(socket, {{"command", "get_hotkeys"}});
    const auto oldToken = initial.value("model_token").toString();
    Check(initial.value("ok").toBool() && !oldToken.isEmpty(), "initial real model token");
    Check(Request(socket, {{"command", "set_model"}, {"index", 1}}).value("ok").toBool(), "select second real model");
    Check(Request(socket, {{"command", "set_model"}, {"index", 0}}).value("ok").toBool(), "restore first real model");
    const auto token = Request(socket, {{"command", "get_hotkeys"}}).value("model_token").toString();
    Check(!token.isEmpty() && token != oldToken, "model replacement invalidates the old token");
    QByteArray requests;
    const QList<QJsonObject> pipeline{
        {{"command", "trigger_hotkey"}, {"id", "binding-42"}, {"model_token", oldToken}},
        {{"command", "trigger_hotkey"}, {"id", "binding-42"}, {"model_token", token}},
        {{"command", "get_hotkeys"}},
        {{"command", "get_render_device"}},
        {{"command", "set_render_device"}, {"device", "ffff:ff:1f.7"}},
        {{"command", "set_render_device"}, {"device", "auto"}},
        {{"command", "get_speech_settings"}},
        {{"command", "set_speech_settings"}, {"api_key", "offline-plugin-secret"}},
        {{"command", "get_speech_settings"}},
        {{"command", "set_speech_settings"}, {"api_key", ""}},
        {{"command", "get_speech_settings"}},
        {{"command", "quit"}}
    };
    for (const auto& request : pipeline) requests += QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n';
    Check(socket.write(requests) == requests.size() && socket.flush(), "enqueue complete command pipeline");
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &app, [] { QCoreApplication::exit(2); });
    deadline.start(5000);
    const int result = app.exec(); // The real desktop plugin ends frames on quit.
    deadline.stop();
    Check(result == 0 && quit.requests == 1, "quit must complete through the actual plugin event loop");
    Check(plugins.Stop(&error), qPrintable(error)); // Same ordering as the real platform.
    Wait([&] { return socket.state() == QLocalSocket::UnconnectedState; }, "transport must close after plugin shutdown");
    const auto response = socket.readAll();
    const auto lines = response.split('\n');
    Check(lines.size() == pipeline.size() + 1 && lines.back().isEmpty(), "every pipelined reply, including quit, survives shutdown");
    auto reply = [&](int index) { return QJsonDocument::fromJson(lines[index]).object(); };
    Check(!reply(0).value("ok").toBool(true) && reply(0).value("error").toString().contains("Model changed"), "old token must be rejected through IPC");
    Check(reply(1).value("ok").toBool(), "current-token hotkey executes through the real desktop adapter");
    const auto status = reply(2);
    const auto bindings = status.value("hotkeys").toArray();
    Check(status.value("model_token") == token && bindings.size() == 2 && bindings[0].toObject().value("active").toBool(), "actual hotkey state and token");
    Check(!bindings[1].toObject().value("active").toBool(true) && !bindings[1].toObject().value("reason").toString().isEmpty(), "duplicate binding stays disabled");
    const auto render = reply(3);
    Check(render.value("ok").toBool() && render.value("device") == "auto" && render.value("active_device") == "auto" &&
        render.value("devices").isArray() && !render.value("restart_required").toBool(true), "actual render-device status");
    Check(!reply(4).value("ok").toBool(true) && reply(4).value("error").toString().contains("unavailable") &&
        reply(4).value("device") == "auto", "unavailable device cannot replace active preference");
    Check(reply(5).value("ok").toBool(), "save valid render-device preference");
    const bool configured[] = {false, true, true, false, false};
    for (int i = 0; i < 5; ++i)
        Check(reply(6 + i).value("ok").toBool() && reply(6 + i).value("configured").toBool() == configured[i], "real speech settings query/save/clear sequence");
    Check(!response.contains("offline-plugin-secret") && !response.contains("api_key"), "wire replies must never expose credentials");
    QFile credentials(configRoot + "/plugin/speech/config.json");
    Check(credentials.open(QIODevice::ReadOnly), "speech owns a separate configuration file");
    Check(QJsonDocument::fromJson(credentials.readAll()).object().value("api_key") == "", "cleared speech credential persists");
    ConfigStore store((configRoot + "/config.json").toStdString());
    ConfigStore::Snapshot saved;
    std::string diagnostic;
    Check(store.Read(saved, diagnostic) && saved.values.at("render_device") == "\"auto\"", "desktop render preference persists");
    Check(!saved.values.count("api_key") && !saved.values.count("minimax_api_key"), "speech credentials cannot leak into desktop configuration");
    Check(reply(11).value("ok").toBool(), "quit acknowledgment is the final actual reply");
    std::cout << "Loaded desktop/IPC/speech command pipeline, model tokens, settings isolation and quit passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
