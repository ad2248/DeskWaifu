#include "SpeechSettings.hpp"
#include "contracts/ConfigStore.hpp"
#include "contracts/SpeechEvents.hpp"
#include "plugin/PluginApi.hpp"
#include "plugin/PluginContext.hpp"
#include "processor/speech/SpeechEndpoint.hpp"
#include <QJsonDocument>

namespace {
constexpr auto SpeakCapability = "speech.speak/v1";
constexpr auto GetSettings = "command.get_speech_settings/v1";
constexpr auto SetSettings = "command.set_speech_settings/v1";
constexpr auto DoubleClick = "interaction.double_click/v1";

class SpeechPlugin final : public IProcessorPlugin
{
public:
    bool Initialize(PluginContext& context, QString* error) override
    {
        _settings.reset(new SpeechSettings(context.ConfigDirectory()));
        QString detail;
        if (!_settings->Refresh(detail)) {
            if (error) *error = QStringLiteral("Unable to read speech settings");
            return false;
        }
        _endpoint = context.CreateEndpoint("service", [this](const EventDelivery& delivery) { Handle(delivery); });
        if (!_endpoint || !_endpoint->Provide(SpeakCapability) || !_endpoint->Provide(GetSettings) ||
            !_endpoint->Provide(SetSettings) || !_endpoint->Subscribe(DoubleClick)) {
            if (error) *error = QStringLiteral("Unable to register speech endpoint");
            return false;
        }
        _speech.reset(new SpeechEndpoint(context.Loop(), *_endpoint, _settings->ApiKey()));
        return true;
    }

    void Stop() noexcept override
    {
        if (_speech) _speech->Stop();
        _speech.reset();
        _settings.reset();
        _endpoint = nullptr;
    }

private:
    void Fail(RequestId request, const QString& error)
    {
        _endpoint->Respond(request, std::make_shared<ProcessorEvent>("operation.failed", nullptr, nullptr,
            QVariantMap{{"error", error}}));
    }

    bool Refresh()
    {
        QString error;
        if (!_settings->Refresh(error)) return false;
        _speech->SetApiKey(_settings->ApiKey());
        return true;
    }

    void Command(const EventDelivery& delivery)
    {
        QJsonObject reply{{"ok", false}, {"configured", _settings->Configured()}};
        const auto respond = [&] {
            _endpoint->Respond(delivery.requestId, std::make_shared<ProcessorEvent>("command.result", nullptr, nullptr,
                QVariantMap{{"json", QString::fromUtf8(QJsonDocument(reply).toJson(QJsonDocument::Compact))}}));
        };
        ConfigStore::Snapshot parsed;
        std::string parseError;
        const auto json = delivery.payload->data.value("json").toString().toUtf8();
        // Check the original document before QJsonObject can collapse duplicate keys.
        if (!ConfigStore::Parse(json.toStdString(), parsed, parseError, false)) {
            reply.insert("error", "Invalid JSON request or duplicate key"); respond(); return;
        }
        const auto object = QJsonDocument::fromJson(json).object();
        const QString expected = delivery.topic == GetSettings ? "get_speech_settings" : "set_speech_settings";
        if (object.value("command") != expected) {
            reply.insert("error", "Speech settings command does not match requested capability"); respond(); return;
        }
        QString error;
        const bool ok = delivery.topic == GetSettings ? _settings->Refresh(error) : _settings->Set(object, error);
        if (ok) _speech->SetApiKey(_settings->ApiKey());
        reply.insert("ok", ok);
        reply.insert("configured", _settings->Configured());
        // Never reflect request text, credentials, parser excerpts or service responses.
        reply.insert("error", ok ? QString{} : QStringLiteral("Unable to read or update speech settings"));
        respond();
    }

    void Handle(const EventDelivery& delivery)
    {
        if (!_speech) return;
        if (delivery.kind == EventDelivery::Kind::Cancellation) {
            if (delivery.topic == SpeakCapability) _speech->Cancel(delivery.requestId);
            return;
        }
        if (delivery.kind == EventDelivery::Kind::Notification) {
            if (delivery.topic == DoubleClick && Refresh() && _settings->Configured())
                _speech->Speak(0, {{"text", QStringLiteral("姐姐，你好喵")}, {"replace", true}});
            return;
        }
        if (delivery.topic == GetSettings || delivery.topic == SetSettings) { Command(delivery); return; }
        const auto& payload = *delivery.payload;
        if (payload.type != SpeechEvents::Speak || payload.data.value("text").metaType().id() != QMetaType::QString ||
            (payload.data.contains("replace") && payload.data.value("replace").metaType().id() != QMetaType::Bool)) {
            Fail(delivery.requestId, "Invalid speech request"); return;
        }
        if (!Refresh()) { Fail(delivery.requestId, "Unable to read speech settings"); return; }
        if (!_settings->Configured()) {
            Fail(delivery.requestId, QStringLiteral("请先在控制面板中设置 MiniMax API Key")); return;
        }
        _speech->Speak(delivery.requestId, payload.data);
    }

    EventEndpoint* _endpoint = nullptr; // Context-owned; invalidated before Stop.
    std::unique_ptr<SpeechSettings> _settings;
    std::unique_ptr<SpeechEndpoint> _speech;
};
}

DESKWAIFU_EXPORT_PLUGIN(SpeechPlugin)
