#include "plugin/PluginApi.hpp"
#include "plugin/PluginContext.hpp"
#include "processor/Processor.hpp"
#include "app/DesktopStartup.hpp"
#include "app/LAppDelegate.hpp"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QTimer>
#include <QDir>
#include <QFile>
#include <csignal>
#include <unistd.h>
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
                for (auto& request : _pending) if (request.id == delivery.requestId) request.cancelled = true;
            } else if (delivery.kind == EventDelivery::Kind::Request && delivery.payload) {
                _pending.push_back({delivery.requestId, delivery.topic, delivery.payload, false});
                if (_pending.size() == 1) Dispatch();
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
    bool HandleEvent(const ProcessorEvent& event) override
    {
        if (event.receiver != this || _pending.empty() ||
            (event.type != "model.result" && event.type != "interaction.result")) return false;
        auto request = std::move(_pending.front());
        _pending.pop_front();
        const QString json = event.type == "model.result"
            ? QString::fromUtf8(QJsonDocument(event.data.value("reply").toJsonObject()).toJson(QJsonDocument::Compact))
            : event.data.value("json").toString();
        if (!request.cancelled) _endpoint->Respond(request.id,
            std::make_shared<ProcessorEvent>("command.result", nullptr, nullptr, QVariantMap{{"json", json}}));
        Dispatch();
        return true;
    }
    void Stop() noexcept override { _pending.clear(); }
private:
    void Dispatch()
    {
        while (!_pending.empty() && _pending.front().cancelled) _pending.pop_front();
        if (_pending.empty()) return;
        const auto& request = _pending.front();
        const auto command = QJsonDocument::fromJson(request.payload->data.value("json").toString().toUtf8())
            .object().value("command").toString();
        if (request.topic != "command." + command + "/v1") {
            auto id = request.id;
            _pending.pop_front();
            _endpoint->Respond(id, std::make_shared<ProcessorEvent>("command.result", nullptr, nullptr,
                QVariantMap{{"json", QStringLiteral("{\"ok\":false,\"error\":\"command capability mismatch\"}")}}));
            Dispatch();
            return;
        }
        Send(ModelCommands.contains(command) ? "interaction.model" : "interaction.command", nullptr, request.payload->data);
    }
    struct Request { RequestId id; QString topic; ProcessorEventPtr payload; bool cancelled; };
    EventEndpoint* _endpoint = nullptr;
    std::deque<Request> _pending;
};

class DesktopPlugin final : public IProcessorPlugin
{
public:
    bool Initialize(PluginContext& context, QString* error) override
    {
        _context = &context;
        for (const auto& argument : QCoreApplication::arguments()) {
            if (argument == "toggle" || argument == "--toggle") { _signal = SIGUSR1; return true; }
            if (argument == "focus" || argument == "--focus") { _signal = SIGUSR2; return true; }
        }
        if (!ConfigureDesktop(error)) {
            QTimer::singleShot(0, QCoreApplication::instance(), [] { QCoreApplication::exit(1); });
            return false;
        }
        _commands.reset(new DesktopCommands(context));
        return _commands->Register();
    }
    bool Start(QString* error) override
    {
        if (_signal) {
            for (const auto& name : QDir("/proc").entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                bool numeric = false;
                const auto pid = name.toInt(&numeric);
                if (!numeric || pid == getpid()) continue;
                QFile command("/proc/" + name + "/comm");
                if (command.open(QIODevice::ReadOnly) && command.readAll().trimmed() == "deskwaifu")
                    ::kill(pid, _signal);
            }
            QTimer::singleShot(0, QCoreApplication::instance(), [] { QCoreApplication::quit(); });
            return true;
        }
        _runtime = LAppDelegate::GetInstance();
        if (!_runtime->Initialize(_context->Loop()) || !_runtime->StartFrames([](bool ok) {
            QCoreApplication::exit(ok ? 0 : 1);
        })) {
            if (error) *error = "Unable to start desktop rendering";
            return false;
        }
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
    int _signal = 0;
    LAppDelegate* _runtime = nullptr;
    std::unique_ptr<DesktopCommands> _commands;
};
}
DESKWAIFU_EXPORT_PLUGIN(DesktopPlugin)
