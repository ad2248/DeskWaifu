#include "app/LAppDelegate.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <deque>

namespace {
bool ProbeStartup(ProcessorEventLoop& loop, const QString& phase)
{
    auto endpoint = loop.CreateEndpoint("desktop-startup-probe", [](const EventDelivery&) {});
    bool done = false;
    RequestStatus status = RequestStatus::TimedOut;
    if (!endpoint->Request("fixture.desktop.early-probe/v1",
        std::make_shared<ProcessorEvent>("fixture.probe", nullptr, nullptr, QVariantMap{{"phase", phase}}),
        [&](const RequestResult& result) { status = result.status; done = true; }, 5000)) return false;
    QElapsedTimer timer; timer.start();
    while (!done && timer.elapsed() < 6000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    return done && (status == RequestStatus::Replied || status == RequestStatus::NoProvider);
}

class CommandBoundary final : public Processor
{
public:
    explicit CommandBoundary(ProcessorEventLoop& loop) : Processor(loop)
    {
        _control = loop.CreateEndpoint("desktop-command-boundary", [this](const EventDelivery& delivery) {
            if (delivery.kind != EventDelivery::Kind::Request || !delivery.payload) return;
            const auto operation = delivery.payload->data.value("operation").toString();
            bool posted = false;
            if (operation == "complete_model") posted = Complete(_models, true);
            else if (operation == "complete_interaction") posted = Complete(_interactions, false);
            else if (operation == "stop") {
                // This already-queued legacy result must be fenced by Stop too.
                posted = Send("model.result", _commands, {{"reply", QJsonObject{{"ok", true}}}});
                _commands->Stop();
            }
            _control->Respond(delivery.requestId, std::make_shared<ProcessorEvent>("fixture.result", nullptr, nullptr,
                QVariantMap{{"models", _seenModels}, {"interactions", _seenInteractions}, {"completed", _completed},
                    {"hidden", _hidden}, {"quits", _quits}, {"posted", posted},
                    {"registered", _loop.Contains(_commands)}}));
        });
        _control->Provide("fixture.desktop/v1");
    }
    bool HandleEvent(const ProcessorEvent& event) override
    {
        const bool model = event.type == "interaction.model";
        if (!model && event.type != "interaction.command") return false;
        _commands = event.sender;
        const auto input = QJsonDocument::fromJson(event.data.value("json").toString().toUtf8()).object();
        (model ? _seenModels : _seenInteractions).push_back(input.value("tag").toString());
        auto& pending = model ? _models : _interactions;
        pending.push_back({event.sender, input});
        if (!model && !input.value("hold").toBool()) Complete(pending, false);
        return true;
    }
private:
    struct Operation { IProcessorPlugin* receiver; QJsonObject input; };
    bool Complete(std::deque<Operation>& pending, bool model)
    {
        if (pending.empty()) return false;
        const auto operation = std::move(pending.front());
        pending.pop_front();
        const auto tag = operation.input.value("tag").toString();
        _completed.push_back(tag);
        const auto command = operation.input.value("command").toString();
        if (command == "toggle_hidden") _hidden = !_hidden;
        if (command == "quit") ++_quits;
        const QJsonObject reply{{"ok", true}, {"tag", tag}, {"hidden", _hidden}};
        if (model) return Send("model.result", operation.receiver, {{"reply", reply}});
        return Send("interaction.result", operation.receiver,
            {{"json", QString::fromUtf8(QJsonDocument(reply).toJson(QJsonDocument::Compact))}});
    }
    std::unique_ptr<EventEndpoint> _control;
    IProcessorPlugin* _commands = nullptr;
    std::deque<Operation> _models, _interactions;
    QStringList _seenModels, _seenInteractions, _completed;
    bool _hidden = false;
    int _quits = 0;
};
}

LAppDelegate* LAppDelegate::GetInstance()
{
    static LAppDelegate instance;
    return &instance;
}
void LAppDelegate::ReleaseInstance() { GetInstance()->_boundary.reset(); }
bool LAppDelegate::Initialize(ProcessorEventLoop& loop)
{
    _loop = &loop;
    if (!ProbeStartup(loop, "initialize")) return false;
    _boundary = std::make_unique<CommandBoundary>(loop);
    return true;
}
bool LAppDelegate::StartFrames(std::function<void(bool)>)
{
    return ProbeStartup(*_loop, "frames");
}
bool LAppDelegate::Release()
{
    if (_boundary) _boundary->Stop();
    return true;
}
