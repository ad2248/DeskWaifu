#include "plugin/PluginApi.hpp"
#include "plugin/PluginContext.hpp"
#include "processor/Processor.hpp"
#include "app/DesktopStartup.hpp"
#include "app/LAppDelegate.hpp"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QTimer>
#include <deque>
#include <cstdio>

namespace {
const QStringList ModelCommands{"set_model", "next_model", "prev_model", "get_hotkeys",
    "trigger_hotkey", "set_expression", "do_motion", "switch_skin"};
const QStringList InteractionCommands{"get_render_device", "set_render_device", "get_mouse_look", "set_mouse_look",
    "get_hover_opacity", "set_hover_opacity", "set_output", "get_transform_save_result", "get_transform_settings",
    "begin_transform_edit", "update_transform_edit", "apply_transform_edit", "cancel_transform_edit",
    "reset_transform_settings", "relocate_crop", "retry_transform_save", "reload_transform_settings",
    "get_available_models", "get_current_model", "get_expressions", "get_motions", "set_mouth_y",
    "set_model_zoom", "set_model_position", "get_model_position", "toggle_hidden", "set_look", "release_source",
    "quit", "get_control_status", "get_status"};

class DesktopCommands final : public Processor
{
public:
    explicit DesktopCommands(PluginContext& context) : Processor(context.Loop())
    {
        _endpoint = context.CreateEndpoint("commands", [this](const EventDelivery& delivery) {
            if (delivery.kind == EventDelivery::Kind::Cancellation) {
                for (auto* pending : {&_modelPending, &_interactionPending})
                    for (auto& request : *pending)
                        if (request.id == delivery.requestId) request.cancelled = true;
            } else if (delivery.kind == EventDelivery::Kind::Request && delivery.payload) {
                if (!_ready) {
                    _endpoint->Respond(delivery.requestId, std::make_shared<ProcessorEvent>("command.result", nullptr, nullptr,
                        QVariantMap{{"json", QStringLiteral("{\"ok\":false,\"error\":\"desktop is not ready\"}")}}));
                    return;
                }
                const auto command = QJsonDocument::fromJson(delivery.payload->data.value("json").toString().toUtf8())
                    .object().value("command").toString();
                if (delivery.topic != "command." + command + "/v1") {
                    _endpoint->Respond(delivery.requestId, std::make_shared<ProcessorEvent>("command.result", nullptr, nullptr,
                        QVariantMap{{"json", QStringLiteral("{\"ok\":false,\"error\":\"command capability mismatch\"}")}}));
                    return;
                }
                const bool model = ModelCommands.contains(command);
                auto& pending = model ? _modelPending : _interactionPending;
                pending.push_back({delivery.requestId, delivery.payload, false});
                if (pending.size() == 1) Dispatch(model);
            }
        });
    }
    bool Register()
    {
        if (!_endpoint) return false;
        for (const auto& command : ModelCommands + InteractionCommands)
            if (!_endpoint->Provide("command." + command + "/v1")) return false;
        return true;
    }
    void SetReady() { _ready = true; }
    bool HandleEvent(const ProcessorEvent& event) override
    {
        if (_stopped || event.receiver != this ||
            (event.type != "model.result" && event.type != "interaction.result")) return false;
        const bool model = event.type == "model.result";
        auto& pending = model ? _modelPending : _interactionPending;
        if (pending.empty()) return false;
        auto request = std::move(pending.front());
        pending.pop_front();
        const QString json = model
            ? QString::fromUtf8(QJsonDocument(event.data.value("reply").toJsonObject()).toJson(QJsonDocument::Compact))
            : event.data.value("json").toString();
        if (!request.cancelled) _endpoint->Respond(request.id,
            std::make_shared<ProcessorEvent>("command.result", nullptr, nullptr, QVariantMap{{"json", json}}));
        Dispatch(model);
        return true;
    }
    void Stop() noexcept override
    {
        _ready = false;
        _stopped = true;
        if (_endpoint) _endpoint->Invalidate();
        _modelPending.clear();
        _interactionPending.clear();
        Processor::Stop();
    }
private:
    void Dispatch(bool model)
    {
        auto& pending = model ? _modelPending : _interactionPending;
        // Only unstarted cancellations may be skipped. The dispatched front is
        // retained until its domain result arrives, even when its caller leaves.
        while (!pending.empty() && pending.front().cancelled) pending.pop_front();
        if (!pending.empty())
            Send(model ? "interaction.model" : "interaction.command", nullptr, pending.front().payload->data);
    }
    struct Request { RequestId id; ProcessorEventPtr payload; bool cancelled; };
    EventEndpoint* _endpoint = nullptr;
    std::deque<Request> _modelPending, _interactionPending;
    bool _ready = false;
    bool _stopped = false;
};

class DesktopPlugin final : public IProcessorPlugin
{
public:
    bool Initialize(PluginContext& context, QString* error) override
    {
        _context = &context;
        if (!ConfigureDesktop(error)) {
            QTimer::singleShot(0, QCoreApplication::instance(), [] { QCoreApplication::exit(1); });
            return false;
        }
        _commands.reset(new DesktopCommands(context));
        return _commands->Register();
    }
    bool Start(QString* error) override
    {
        _runtime = LAppDelegate::GetInstance();
        if (!_runtime->Initialize(_context->Loop()) || !_runtime->StartFrames([](bool ok) {
            QCoreApplication::exit(ok ? 0 : 1);
        })) {
            if (error) *error = "Unable to start desktop rendering";
            return false;
        }
        _commands->SetReady();
        return true;
    }
    void Stop() noexcept override
    {
        if (_commands) _commands->Stop();
        _commands.reset();
        if (_runtime) {
            const bool released = _runtime->Release();
            if (!released) _context->ReportFailure("Could not restore GPU context for teardown");
            if (released) std::fprintf(stdout, "[APP]GPU resources released before window teardown\n");
            else std::fprintf(stderr, "[APP]Could not restore GPU context for teardown\n");
            LAppDelegate::ReleaseInstance();
            _runtime = nullptr;
        }
        _context = nullptr;
    }
private:
    PluginContext* _context = nullptr;
    LAppDelegate* _runtime = nullptr;
    std::unique_ptr<DesktopCommands> _commands;
};
}
DESKWAIFU_EXPORT_PLUGIN(DesktopPlugin)
