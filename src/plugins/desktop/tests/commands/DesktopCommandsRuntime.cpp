#include "app/LAppDelegate.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <deque>

namespace {
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
    _boundary = std::make_unique<CommandBoundary>(loop);
    return true;
}
bool LAppDelegate::Release()
{
    _boundary->Stop();
    return true;
}
