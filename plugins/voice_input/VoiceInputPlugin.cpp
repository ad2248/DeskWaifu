#include "VoiceInputSettings.hpp"
#include "private/VoiceInputController.hpp"
#include "private/VoiceInputNotifier.hpp"
#include "private/audio/AudioCapture.hpp"
#include "private/asr/NativeAsrClient.hpp"
#include "contracts/ConfigStore.hpp"
#include "contracts/VoiceInputEvents.hpp"
#include "plugin/PluginApi.hpp"
#include "plugin/PluginContext.hpp"
#include <QJsonDocument>
#include <QRegularExpression>

namespace {
const QStringList Commands{"start_voice_input", "stop_voice_input", "toggle_voice_input", "cancel_voice_input",
    "get_voice_input_status", "get_voice_input_result", "get_voice_input_settings", "set_voice_input_settings",
    "list_voice_input_devices", "retry_voice_input_copy"};
const QStringList Capabilities{VoiceInputEvents::Start, VoiceInputEvents::Stop, VoiceInputEvents::Toggle,
    VoiceInputEvents::Cancel, VoiceInputEvents::Status, VoiceInputEvents::Result, VoiceInputEvents::Recognize};

class VoiceInputPlugin final : public IProcessorPlugin
{
public:
    bool Initialize(PluginContext& context, QString* error) override
    {
        _settings = std::make_unique<VoiceInputSettings>(context.ConfigDirectory());
        QString detail;
        if (!_settings->Refresh(detail)) {
            if (error) *error = "Unable to read voice input settings";
            return false;
        }
        _endpoint = context.CreateEndpoint("service", [this](const EventDelivery& delivery) { Handle(delivery); });
        if (!_endpoint) return false;
        for (const auto& capability : Capabilities) if (!_endpoint->Provide(capability)) return false;
        for (const auto& command : Commands) if (!_endpoint->Provide("command." + command + "/v1")) return false;
        _capture = std::make_unique<AudioCapture>();
        _asr = std::make_unique<NativeAsrClient>();
        _controller = std::make_unique<VoiceInputController>(*_endpoint, *_settings, *_capture, *_asr);
        return true;
    }
    bool Start(QString*) override
    {
        _notifier = std::make_unique<VoiceInputNotifier>();
        QObject::connect(_controller.get(), &VoiceInputController::StateChanged,
            _notifier.get(), &VoiceInputNotifier::ShowState);
        _ready = true; return true;
    }
    void Stop() noexcept override
    {
        _ready = false;
        if (_controller) _controller->Shutdown();
        _notifier.reset(); _controller.reset(); _asr.reset(); _capture.reset(); _settings.reset(); _endpoint = nullptr;
    }
private:
    static QJsonObject Error(const QString& code, const QString& message)
    { return {{"ok", false}, {"error_code", code}, {"error", message}}; }
    static bool SessionValid(const QJsonObject& object)
    {
        const auto value = object.value("session_id");
        static const QRegularExpression pattern("^[1-9][0-9]{0,19}$");
        bool ok = false;
        return value.isString() && pattern.match(value.toString()).hasMatch() && value.toString().toULongLong(&ok) && ok;
    }
    QJsonObject Execute(const QString& operation, const QJsonObject& object)
    {
        if (operation == "start_voice_input") return _controller->StartRecording();
        if (operation == "toggle_voice_input") return _controller->Toggle();
        if (operation == "get_voice_input_status") return _controller->Status();
        if (operation == "stop_voice_input" || operation == "cancel_voice_input" ||
            operation == "get_voice_input_result" || operation == "retry_voice_input_copy") {
            if (!SessionValid(object)) return Error("invalid_session", "session_id 必须为有效的会话号字符串");
            const auto session = object.value("session_id").toString();
            if (operation == "stop_voice_input") return _controller->StopRecording(session);
            if (operation == "cancel_voice_input") return _controller->Cancel(session);
            if (operation == "retry_voice_input_copy") return _controller->RetryCopy(session);
            return _controller->Result(session);
        }
        return Error("invalid_request", "不支持的语音输入操作");
    }
    void Command(const EventDelivery& delivery)
    {
        const auto respond = [&](const QJsonObject& response) {
            _endpoint->Respond(delivery.requestId, std::make_shared<ProcessorEvent>("command.result", nullptr, nullptr,
                QVariantMap{{"json", QString::fromUtf8(QJsonDocument(response).toJson(QJsonDocument::Compact))}}));
        };
        const auto value = delivery.payload->data.value("json");
        ConfigStore::Snapshot parsed; std::string parseError;
        if (delivery.payload->type != "command.invoke" || value.metaType().id() != QMetaType::QString ||
            !ConfigStore::Parse(value.toString().toStdString(), parsed, parseError, false)) {
            respond(Error("invalid_request", "无效 JSON 请求或重复键")); return;
        }
        const auto object = QJsonDocument::fromJson(value.toString().toUtf8()).object();
        const auto name = object.value("command").toString();
        if (!Commands.contains(name) || delivery.topic != "command." + name + "/v1") {
            respond(Error("invalid_request", "命令与能力不匹配")); return;
        }
        if (name == "get_voice_input_settings" || name == "set_voice_input_settings") {
            QString error;
            const bool ok = name == "get_voice_input_settings" ? _settings->Refresh(error)
                : _settings->Set(value.toString().toStdString(), error);
            if (!ok) { respond(Error("settings_failed", "无法读取或保存语音输入配置，请检查字段及文件")); return; }
            auto result = _settings->PublicView(); result.insert("ok", true); respond(result);
        } else if (name == "list_voice_input_devices") {
            respond({{"ok", true}, {"devices", AudioCapture::ListDevices()}});
        } else respond(Execute(name, object));
    }
    void Handle(const EventDelivery& delivery)
    {
        if (!_controller || !_endpoint) return;
        if (delivery.kind == EventDelivery::Kind::Cancellation) {
            if (delivery.topic == VoiceInputEvents::Recognize) _controller->CancelRequest(delivery.requestId);
            return;
        }
        if (delivery.kind != EventDelivery::Kind::Request) return;
        const bool command = delivery.topic.startsWith("command.");
        if (!_ready || !delivery.payload) {
            const auto error = Error("unavailable", "语音输入服务未就绪");
            _endpoint->Respond(delivery.requestId, command
                ? std::make_shared<ProcessorEvent>("command.result", nullptr, nullptr,
                    QVariantMap{{"json", QString::fromUtf8(QJsonDocument(error).toJson(QJsonDocument::Compact))}})
                : std::make_shared<ProcessorEvent>(VoiceInputEvents::ResultType, nullptr, nullptr, error.toVariantMap()));
            return;
        }
        if (command) { Command(delivery); return; }
        if (delivery.topic == VoiceInputEvents::Recognize && delivery.payload->type == VoiceInputEvents::Audio &&
            delivery.payload->data.value("pcm").metaType().id() == QMetaType::QByteArray) {
            _controller->Recognize(delivery.requestId, delivery.payload->data.value("pcm").toByteArray()); return;
        }
        const QStringList controlCapabilities{VoiceInputEvents::Start, VoiceInputEvents::Stop, VoiceInputEvents::Toggle,
            VoiceInputEvents::Cancel, VoiceInputEvents::Status, VoiceInputEvents::Result};
        const QStringList operations{"start_voice_input", "stop_voice_input", "toggle_voice_input", "cancel_voice_input",
            "get_voice_input_status", "get_voice_input_result"};
        const auto index = controlCapabilities.indexOf(delivery.topic);
        auto response = Error("invalid_request", "无效的语音输入消息");
        if (index >= 0 && delivery.payload->type == VoiceInputEvents::Control) {
            const auto session = delivery.payload->data.value("session_id");
            if (!session.isValid() || session.metaType().id() == QMetaType::QString)
                response = Execute(operations.at(index), QJsonObject::fromVariantMap(delivery.payload->data));
        }
        _endpoint->Respond(delivery.requestId, std::make_shared<ProcessorEvent>(VoiceInputEvents::ResultType,
            nullptr, nullptr, response.toVariantMap()));
    }
    EventEndpoint* _endpoint = nullptr;
    std::unique_ptr<VoiceInputSettings> _settings;
    std::unique_ptr<AudioCapture> _capture;
    std::unique_ptr<NativeAsrClient> _asr;
    std::unique_ptr<VoiceInputController> _controller;
    std::unique_ptr<VoiceInputNotifier> _notifier;
    bool _ready = false;
};
}
DESKWAIFU_EXPORT_PLUGIN(VoiceInputPlugin)
