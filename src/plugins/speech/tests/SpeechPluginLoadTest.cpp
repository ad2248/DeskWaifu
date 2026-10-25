#include "plugin/PluginManager.hpp"
#include "contracts/SpeechEvents.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <sys/stat.h>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Wait(const std::function<bool()>& done)
{
    QElapsedTimer timer; timer.start();
    while (!done() && timer.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    Check(done(), "asynchronous speech plugin operation timed out");
}
void Drain() { for (int i = 0; i < 100; ++i) QCoreApplication::processEvents(); }
QByteArray Read(const QString& path)
{ QFile file(path); Check(file.open(QIODevice::ReadOnly), "read configuration"); return file.readAll(); }
void Write(const QString& path, const QByteArray& bytes)
{
    QFile file(path); Check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "write configuration fixture");
    Check(file.write(bytes) == bytes.size(), "write full fixture");
}
RequestResult Request(EventEndpoint& client, const QString& capability, ProcessorEventPtr payload)
{
    std::optional<RequestResult> result;
    Check(client.Request(capability, std::move(payload), [&](const RequestResult& reply) { result = reply; }, 1000) != 0, "request accepted");
    Wait([&] { return result.has_value(); });
    return *result;
}
QJsonObject Command(EventEndpoint& client, const QString& command, const QString& json, const QString& secret = {})
{
    const auto result = Request(client, "command." + command + "/v1",
        std::make_shared<ProcessorEvent>("command.request", nullptr, nullptr,
            QVariantMap{{"json", json}, {"connection", "test-connection"}}));
    Check(result.status == RequestStatus::Replied && result.payload, "command responds through capability");
    const auto text = result.payload->data.value("json").toString().toUtf8();
    Check(!text.contains('\n') && !text.contains("api_key") && (secret.isEmpty() || !text.contains(secret.toUtf8())), "compact reply never reflects credentials");
    const auto document = QJsonDocument::fromJson(text);
    Check(document.isObject() && document.object().value("configured").isBool(), "settings reply shape");
    return document.object();
}
}

int main(int argc, char** argv)
try {
    QCoreApplication app(argc, argv);
    Check(argc == 2, "usage: speech-plugin-load-test /path/to/speech.so");
    QTemporaryDir directory;
    Check(directory.isValid(), "temporary directory");
    const auto root = directory.path() + "/modules";
    const auto configRoot = directory.path() + "/configuration";
    const auto path = configRoot + "/plugin/speech/config.json";
    Check(QDir().mkpath(root + "/speech"), "create exact discovery directory");
    Check(QFile::copy(QString::fromLocal8Bit(argv[1]), root + "/speech/speech.so"), "copy real speech MODULE");
    ProcessorEventLoop loop;
    PluginManager manager(loop, root, configRoot);
    QString error;
    Check(manager.Start(&error), qPrintable(error));
    Check(manager.LoadedPlugins() == QStringList{"speech"}, "real module loaded with matching ABI");
    auto client = loop.CreateEndpoint("test/client", [](const EventDelivery&) {});
    auto settings = Command(*client, "get_speech_settings", R"({"command":"get_speech_settings"})");
    Check(settings.value("ok").toBool() && !settings.value("configured").toBool(), "missing credentials allow initialization");
    Check(!QFile::exists(path), "reading absent settings must not create or overwrite configuration");
    auto speak = std::make_shared<ProcessorEvent>(SpeechEvents::Speak, nullptr, nullptr,
        QVariantMap{{"text", "offline test"}, {"replace", true}});
    auto speech = Request(*client, "speech.speak/v1", speak);
    Check(speech.status == RequestStatus::Replied && speech.payload->type == "operation.failed" &&
          !speech.payload->data.value("error").toString().isEmpty(), "missing credentials fail at domain level without network or audio");
    Check(client->Notify("interaction.double_click/v1", std::make_shared<ProcessorEvent>()), "double-click notification accepted");
    Drain(); Check(!QFile::exists(path), "unconfigured greeting neither writes config nor requests credentials");

    const QString secret = "speech-plugin-secret-do-not-echo";
    settings = Command(*client, "set_speech_settings",
        "{\"command\":\"set_speech_settings\",\"api_key\":\"" + secret + "\"}", secret);
    Check(settings.value("ok").toBool() && settings.value("configured").toBool(), "set config through dynamically resolved command");
    struct stat info;
    Check(::stat(path.toLocal8Bit().constData(), &info) == 0 && (info.st_mode & 0777) == 0600, "plugin configuration path and private 0600 permissions");
    Check(QJsonDocument::fromJson(Read(path)).object().value("api_key") == secret, "only plugin's private file stores the key");
    Check(!QFile::exists(configRoot + "/config.json") && !QFile::exists(configRoot + "/speech.json"), "host configuration is untouched");
    settings = Command(*client, "get_speech_settings", R"({"command":"get_speech_settings"})", secret);
    Check(settings.value("ok").toBool() && settings.value("configured").toBool(), "get returns configured state, not the credential");

    const auto validFile = Read(path);
    for (const auto& json : {
        QStringLiteral(R"({"command":"set_speech_settings","api_key":false})"),
        QStringLiteral(R"({"command":"set_speech_settings","api_key":"first","api_key":"second"})"),
        QStringLiteral(R"({"command":"set_speech_settings","api_key":"bad\nkey"})"),
        QStringLiteral(R"({"command":"get_speech_settings","api_key":"wrong-capability"})")}) {
        settings = Command(*client, "set_speech_settings", json, secret);
        Check(!settings.value("ok").toBool() && Read(path) == validFile, "invalid command cannot alter saved configuration");
    }
    settings = Command(*client, "set_speech_settings", R"({"command":"set_speech_settings","api_key":""})", secret);
    Check(settings.value("ok").toBool() && !settings.value("configured").toBool(), "empty key clears credentials");
    Check(!Read(path).contains(secret.toUtf8()), "clear removes old credential from current file");
    Check(::stat(path.toLocal8Bit().constData(), &info) == 0 && (info.st_mode & 0777) == 0600, "replacement retains private permissions");

    const QByteArray corrupt = "{broken-private-configuration";
    Write(path, corrupt);
    settings = Command(*client, "get_speech_settings", R"({"command":"get_speech_settings"})");
    Check(!settings.value("ok").toBool(), "corruption reported by get");
    settings = Command(*client, "set_speech_settings", R"({"command":"set_speech_settings","api_key":"must-not-overwrite"})");
    Check(!settings.value("ok").toBool() && Read(path) == corrupt, "corrupted settings are never silently replaced");
    speech = Request(*client, "speech.speak/v1", speak);
    Check(speech.status == RequestStatus::Replied && speech.payload->type == "operation.failed", "corrupt settings cannot use stale cached credentials");
    manager.Stop(); Drain();
    Check(!manager.Start(&error) && !error.isEmpty() && Read(path) == corrupt, "corrupt startup rolls back without repairing or deleting private data");
    Write(path, "{\"api_key\":\"\"}\n");
    Check(manager.Start(&error), "restart after explicit config repair has no leaked endpoint registrations");

    int callbacks = 0;
    RequestStatus stopped = RequestStatus::Replied;
    Check(client->Request("speech.speak/v1", speak, [&](const RequestResult& result) { ++callbacks; stopped = result.status; }) != 0, "queue work immediately before Stop");
    manager.Stop(); Drain();
    Check(callbacks == 1 && stopped == RequestStatus::EndpointUnavailable, "Stop fails pending request once without dangling plugin callback");
    speech = Request(*client, "speech.speak/v1", speak);
    Check(speech.status == RequestStatus::NoProvider, "Stop withdraws capability");
    Check(manager.Start(&error), "second restart succeeds");
    auto disappearing = loop.CreateEndpoint("test/disappearing", [](const EventDelivery&) {});
    disappearing->Request("speech.speak/v1", speak, [&](const RequestResult&) { ++callbacks; });
    disappearing.reset(); manager.Stop(); Drain();
    Check(callbacks == 1, "destroyed requester and plugin cannot receive late callbacks");
    std::cout << "Real speech MODULE loading and private configuration contracts passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
