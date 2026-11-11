#include "contracts/VoiceInputEvents.hpp"
#include "plugin/PluginManager.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <sys/stat.h>

namespace {
const QString Secret = QStringLiteral("voice-input-plugin-secret-do-not-echo");
void Check(bool value, const char* message)
{
    if (!value) {
        throw std::runtime_error(message);
    }
}
void Wait(const std::function<bool()>& done)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    Check(done(), "asynchronous voice input plugin operation timed out");
}
void Drain()
{
    for (int i = 0; i < 100; ++i) {
        QCoreApplication::processEvents();
    }
}
QByteArray Read(const QString& path)
{
    QFile file(path);
    Check(file.open(QIODevice::ReadOnly), "read private configuration");
    return file.readAll();
}
void PrivateFile(const QString& path)
{
    struct stat info;
    Check(::stat(QFile::encodeName(path).constData(), &info) == 0 && (info.st_mode & 0777) == 0600, "voice input configuration is owner-only 0600");
}
RequestResult Request(EventEndpoint& client, const QString& capability, ProcessorEventPtr payload)
{
    std::optional<RequestResult> result;
    Check(client.Request(capability, std::move(payload), [&](const RequestResult& reply) { result = reply; }, 1000) != 0, "request accepted");
    Wait([&] { return result.has_value(); });
    return *result;
}
ProcessorEventPtr Control() { return std::make_shared<ProcessorEvent>(VoiceInputEvents::Control); }
QJsonObject Command(EventEndpoint& client, const QString& command, const QString& json)
{
    const auto result = Request(client, "command." + command + "/v1",
        std::make_shared<ProcessorEvent>("command.invoke", nullptr, nullptr, QVariantMap { { "json", json }, { "connection", "voice-input-test-connection" } }));
    Check(result.status == RequestStatus::Replied && result.payload && result.payload->type == "command.result", "real command.invoke envelope receives command.result");
    const auto value = result.payload->data.value("json");
    Check(value.metaType().id() == QMetaType::QString, "command reply json is a string");
    const auto text = value.toString().toUtf8();
    Check(!text.contains('\n') && !text.contains("api_key") && !text.contains(Secret.toUtf8()), "compact command reply never reflects the API key");
    const auto document = QJsonDocument::fromJson(text);
    Check(document.isObject() && document.object().value("ok").isBool(), "command reply shape");
    return document.object();
}
QJsonObject Command(EventEndpoint& client, const QString& command)
{
    return Command(client, command, QString::fromUtf8(QJsonDocument(QJsonObject { { "command", command } }).toJson(QJsonDocument::Compact)));
}
void Idle(const QJsonObject& status)
{
    Check(status.value("ok").toBool() && status.value("state").isString() && status.value("state") == "idle" && status.value("phase") == "idle",
        "initial public three-state status is idle, not an internal phase");
    Check(
        status.value("session_id").isString() && status.value("session_id") == "0" && status.value("elapsed_ms").toDouble(-1) == 0 && status.value("copy_state") == "not_requested",
        "idle status has no session, elapsed audio or clipboard work");
    Check(!status.contains("text") && !status.contains("api_key"), "status exposes neither transcript nor key");
}
}

int main(int argc, char** argv)
try {
    QCoreApplication app(argc, argv);
    Check(argc == 2, "usage: voice-input-plugin-load-test /path/to/voice_input.so");
    QTemporaryDir directory;
    Check(directory.isValid(), "temporary directory");
    const auto root = directory.path() + "/modules";
    const auto configRoot = directory.path() + "/configuration";
    const auto path = configRoot + "/plugin/voice_input/config.json";
    Check(QDir().mkpath(root + "/voice_input"), "create exact discovery directory");
    Check(QFile::copy(QString::fromLocal8Bit(argv[1]), root + "/voice_input/voice_input.so"), "copy real voice input MODULE");
    ProcessorEventLoop loop;
    PluginManager manager(loop, root, configRoot);
    QString error;
    Check(manager.Start(&error), qPrintable(error));
    Check(manager.LoadedPlugins() == QStringList { "voice_input" }, "real module loads without credentials");
    auto client = loop.CreateEndpoint("test/voice-input-client", [](const EventDelivery&) { });
    auto settings = Command(*client, "get_voice_input_settings");
    Check(settings.value("ok").toBool() && settings.value("configured").isBool() && !settings.value("configured").toBool(), "absent key permits initialization");
    Check(!QFile::exists(path), "settings query does not create an absent file");
    Idle(Command(*client, "get_voice_input_status"));
    auto start = Command(*client, "start_voice_input");
    Check(!start.value("ok").toBool() && start.value("error_code") == "not_configured", "unconfigured command start fails before opening a device");
    auto domain = Request(*client, VoiceInputEvents::Start, Control());
    Check(domain.status == RequestStatus::Replied && domain.payload && domain.payload->type == VoiceInputEvents::ResultType && !domain.payload->data.value("ok").toBool()
            && domain.payload->data.value("error_code").toString() == "not_configured",
        "unconfigured domain start fails without device or cloud access");
    Idle(Command(*client, "get_voice_input_status"));

    settings = Command(
        *client, "set_voice_input_settings", "{\"command\":\"set_voice_input_settings\",\"endpoint\":\"https://asr.example.invalid/generation\",\"api_key\":\"" + Secret + "\"}");
    Check(settings.value("ok").toBool() && settings.value("configured").toBool() && settings.value("endpoint") == "https://asr.example.invalid/generation",
        "set endpoint and key through real command envelope");
    PrivateFile(path);
    settings = Command(*client, "set_voice_input_settings", R"({"command":"set_voice_input_settings","endpoint":"https://other.example.invalid/asr","max_recording_seconds":90})");
    Check(settings.value("ok").toBool() && settings.value("configured").toBool() && settings.value("endpoint") == "https://other.example.invalid/asr"
            && settings.value("max_recording_seconds") == 90,
        "partial update changes public fields while preserving configured key");
    const auto saved = Read(path);
    const auto savedObject = QJsonDocument::fromJson(saved).object();
    Check(savedObject.value("api_key") == Secret && savedObject.value("endpoint") == "https://other.example.invalid/asr",
        "partial update preserves actual private key, not just configured flag");
    PrivateFile(path);
    Check(!QFile::exists(configRoot + "/config.json"), "host configuration remains untouched");
    settings = Command(*client, "get_voice_input_settings");
    Check(settings.value("ok").toBool() && settings.value("configured").toBool() && settings.value("endpoint") == "https://other.example.invalid/asr",
        "public readback contains endpoint but no key");

    for (const auto& json : { QStringLiteral(R"({"command":"set_voice_input_settings","api_key":"first","api_key":"second"})"),
             QStringLiteral(R"({"command":"set_voice_input_settings","endpoint":"http://asr.example.invalid"})"),
             QStringLiteral(R"({"command":"set_voice_input_settings","endpoint":"file:///private"})"), QStringLiteral(R"({"command":"set_voice_input_settings","endpoint":false})"),
             QStringLiteral(R"({"command":"set_voice_input_settings","api_key":123})"), QStringLiteral(R"({"command":"set_voice_input_settings","device_id":false})"),
             QStringLiteral(R"({"command":"set_voice_input_settings","max_recording_seconds":"60"})"),
             QStringLiteral(R"({"command":"set_voice_input_settings","max_recording_seconds":1.5})"), QStringLiteral(R"({"command":"get_voice_input_settings"})") }) {
        const auto reply = Command(*client, "set_voice_input_settings", json);
        Check(!reply.value("ok").toBool() && Read(path) == saved, "duplicate keys, invalid types/schemes and wrong command cannot mutate configuration");
    }
    for (const auto& pcm : { QByteArray(), QByteArray(1, '\0'), QByteArray(VoiceInputEvents::MaxAudioBytes + 2, '\0') }) {
        const auto result
            = Request(*client, VoiceInputEvents::Recognize, std::make_shared<ProcessorEvent>(VoiceInputEvents::Audio, nullptr, nullptr, QVariantMap { { "pcm", pcm } }));
        Check(result.status == RequestStatus::Replied && result.payload && result.payload->type == VoiceInputEvents::ResultType && !result.payload->data.value("ok").toBool()
                && result.payload->data.value("error_code").toString() == "invalid_audio",
            "invalid PCM rejected before any ASR request even with credentials configured");
    }
    const auto wrongType = Request(
        *client, VoiceInputEvents::Recognize, std::make_shared<ProcessorEvent>(VoiceInputEvents::Audio, nullptr, nullptr, QVariantMap { { "pcm", QStringLiteral("not bytes") } }));
    Check(wrongType.status == RequestStatus::Replied && wrongType.payload && wrongType.payload->data.value("error_code").toString() == "invalid_request",
        "PCM QVariant type is strictly checked");
    Idle(Command(*client, "get_voice_input_status"));
    const auto retry = Command(*client, "retry_voice_input_copy", R"({"command":"retry_voice_input_copy","session_id":"1"})");
    Check(!retry.value("ok").toBool() && retry.value("error_code") == "stale_session", "retry command resolves to backend and rejects missing result without clipboard access");

    int callbacks = 0;
    RequestStatus stopped = RequestStatus::Replied;
    Check(client->Request(VoiceInputEvents::Status, Control(),
              [&](const RequestResult& result) {
                  ++callbacks;
                  stopped = result.status;
              })
            != 0,
        "queue safe status work immediately before Stop");
    Check(manager.Stop(&error), qPrintable(error));
    Drain();
    Check(callbacks == 1 && stopped == RequestStatus::EndpointUnavailable, "Stop fails pending request exactly once");
    Check(manager.LoadedPlugins().isEmpty(), "Stop removes loaded instances");
    for (const auto* capability : { VoiceInputEvents::Start, VoiceInputEvents::Stop, VoiceInputEvents::Toggle, VoiceInputEvents::Cancel, VoiceInputEvents::Status,
             VoiceInputEvents::Result, VoiceInputEvents::Recognize, "command.start_voice_input/v1", "command.stop_voice_input/v1", "command.toggle_voice_input/v1",
             "command.cancel_voice_input/v1", "command.get_voice_input_status/v1", "command.get_voice_input_result/v1", "command.get_voice_input_settings/v1",
             "command.set_voice_input_settings/v1", "command.list_voice_input_devices/v1", "command.retry_voice_input_copy/v1" }) {
        Check(Request(*client, capability, Control()).status == RequestStatus::NoProvider, "Stop withdraws every plugin capability");
    }
    Check(manager.Start(&error), qPrintable(error));
    Check(manager.LoadedPlugins() == QStringList { "voice_input" }, "restart has no leaked registrations");
    Idle(Command(*client, "get_voice_input_status"));
    settings = Command(*client, "get_voice_input_settings");
    Check(settings.value("ok").toBool() && settings.value("configured").toBool() && settings.value("endpoint") == "https://other.example.invalid/asr" && Read(path) == saved,
        "restart reloads private settings without modifying them or exposing key");
    PrivateFile(path);
    Check(manager.Stop(&error), qPrintable(error));
    Drain();
    Check(callbacks == 1, "restart and shutdown deliver no late completion");
    std::cout << "Real voice input MODULE loading, offline commands and private configuration contracts passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
