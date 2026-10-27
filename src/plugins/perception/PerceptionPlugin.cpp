#include "plugin/PluginApi.hpp"
#include "plugin/PluginContext.hpp"
#include "processor/Processor.hpp"
#include "contracts/ConfigStore.hpp"
#include "processor/image/ImageProcessor.hpp"
#include "processor/ocr/OcrProcessor.hpp"
#include "processor/screenshot/ScreenshotProcessor.hpp"
#include <QGuiApplication>
#include <QClipboard>
#include <QJsonDocument>
#include <QJsonObject>
#include <deque>

namespace {
class PerceptionRuntime final : public Processor
{
public:
    explicit PerceptionRuntime(ProcessorEventLoop& loop) : Processor(loop) {}
    bool Initialize(PluginContext& context, QString*) override
    {
        _context = &context;
        _commands = context.CreateEndpoint("commands", [this](const EventDelivery& delivery) {
            if (delivery.kind != EventDelivery::Kind::Request || !delivery.payload) return;
            const auto json = delivery.payload->data.value("json").toString();
            ConfigStore::Snapshot parsed;
            std::string error;
            const auto command = QJsonDocument::fromJson(json.toUtf8()).object().value("command").toString();
            if (!ConfigStore::Parse(json.toStdString(), parsed, error, false) ||
                delivery.topic != "command." + command + "/v1") {
                _commands->Respond(delivery.requestId, std::make_shared<ProcessorEvent>("command.result", nullptr, nullptr,
                    QVariantMap{{"json", QStringLiteral("{\"ok\":false,\"error\":\"invalid command request\"}")}}));
                return;
            }
            if (!_ocr) {
                _commands->Respond(delivery.requestId, std::make_shared<ProcessorEvent>("command.result", nullptr, nullptr,
                    QVariantMap{{"json", QStringLiteral("{\"ok\":false,\"error\":\"perception is not ready\"}")}}));
                return;
            }
            _requests.push_back(delivery.requestId);
            Send("ocr.command", _ocr.get(), delivery.payload->data);
        });
        if (!_commands) return false;
        for (const QString& command : {"get_ocr_status", "ocr_recognize", "get_ocr_result", "cancel_ocr"})
            if (!_commands->Provide("command." + command + "/v1")) return false;
        return true;
    }
    bool Start(QString*) override
    {
        _images.reset(new ImageProcessor(_loop, [] { return QGuiApplication::clipboard()->image(); }, _context));
        _ocr.reset(new OcrProcessor(*_context));
        _screenshots.reset(new ScreenshotProcessor(_loop));
        return true;
    }
    bool HandleEvent(const ProcessorEvent& event) override
    {
        if (event.type != "ocr.result" || event.receiver != this || _requests.empty()) return false;
        const auto id = _requests.front(); _requests.pop_front();
        _commands->Respond(id, std::make_shared<ProcessorEvent>("command.result", nullptr, nullptr, event.data));
        return true;
    }
    void Stop() noexcept override
    {
        if (_images) _images->Stop();
        if (_ocr) _ocr->Stop();
        if (_screenshots) _screenshots->Stop();
        _images.reset(); _ocr.reset(); _screenshots.reset();
        _requests.clear(); _commands = nullptr;
    }
private:
    std::unique_ptr<ImageProcessor> _images;
    PluginContext* _context = nullptr;
    std::unique_ptr<OcrProcessor> _ocr;
    std::unique_ptr<ScreenshotProcessor> _screenshots;
    EventEndpoint* _commands = nullptr;
    std::deque<RequestId> _requests;
};

class PerceptionPlugin final : public IProcessorPlugin
{
public:
    bool Initialize(PluginContext& context, QString* error) override
    {
        _runtime.reset(new PerceptionRuntime(context.Loop()));
        return _runtime->Initialize(context, error);
    }
    bool Start(QString* error) override { return _runtime->Start(error); }
    void Stop() noexcept override { if (_runtime) _runtime->Stop(); _runtime.reset(); }
private:
    std::unique_ptr<PerceptionRuntime> _runtime;
};
}
DESKWAIFU_EXPORT_PLUGIN(PerceptionPlugin)
