#include "processor/command/CommandProcessor.hpp"
#include "processor/ipc/IpcProcessor.hpp"
#include <QCoreApplication>
#include <iostream>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::cerr << __LINE__ << ": " << #c << '\n'; return 1; } } while (false)

// Drive the SDK's manager-only barrier without linking platform implementation.
class PluginManager
{
public:
    static std::unique_ptr<PluginContext> Create(ProcessorEventLoop& loop)
    { return std::unique_ptr<PluginContext>(new PluginContext(loop, "ipc-lifecycle", {})); }
    static void Activate(PluginContext& context) { context.Activate(); }
    static void Invalidate(PluginContext& context) { context.Invalidate(); }
};

namespace {
void Drain() { for (int i = 0; i < 40; ++i) QCoreApplication::processEvents(); }

class Receiver final : public Processor
{
public:
    using Processor::Processor;
    std::vector<ProcessorEvent> replies;
    std::vector<QString> disconnected;
    bool HandleEvent(const ProcessorEvent& event) override
    {
        if (event.type == "ipc.command.result") { replies.push_back(event); return true; }
        if (event.type == "command.disconnected") disconnected.push_back(event.data.value("connection").toString());
        return false;
    }
};

class Stopper final : public Processor
{
public:
    Stopper(ProcessorEventLoop& loop, CommandProcessor& gateway) : Processor(loop), _gateway(gateway) {}
    bool HandleEvent(const ProcessorEvent& event) override
    {
        if (event.type != "test.stop.gateway") return false;
        _gateway.Stop();
        return true;
    }
private:
    CommandProcessor& _gateway;
};

ProcessorEvent Request(IProcessorPlugin* sender, CommandProcessor* gateway, const QString& connection)
{
    return {"command.request", sender, gateway,
        {{"json", QStringLiteral(R"({"command":"lifecycle"})")}, {"connection", connection}}};
}

ProcessorEventPtr Result()
{
    return std::make_shared<ProcessorEvent>("command.result", nullptr, nullptr,
        QVariantMap{{"json", QStringLiteral(R"({"ok":true})")}});
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    {
        ProcessorEventLoop loop;
        auto context = PluginManager::Create(loop);
        auto gateway = std::make_unique<CommandProcessor>(loop, context.get());
        Receiver receiver(loop);
        std::vector<RequestId> requests;
        int cancelled = 0;
        auto provider = loop.CreateEndpoint("lifecycle-provider", [&](const EventDelivery& event) {
            if (event.kind == EventDelivery::Kind::Request) requests.push_back(event.requestId);
            if (event.kind == EventDelivery::Kind::Cancellation) ++cancelled;
        });
        CHECK(provider->Provide("command.lifecycle/v1"));

        // Production endpoints cannot issue requests during Initialize.
        CHECK(loop.Post(Request(&receiver, gateway.get(), "before-start")));
        Drain();
        CHECK(requests.empty() && receiver.replies.size() == 1);
        CHECK(receiver.replies.back().data.value("json").toString().contains("gateway unavailable"));
        receiver.replies.clear();
        PluginManager::Activate(*context);
        CHECK(loop.Post(Request(&receiver, gateway.get(), "completion-queued")));
        Drain();
        CHECK(requests.size() == 1);
        CHECK(provider->Respond(requests.front(), Result()));
        // Leave one completion and one undelivered request in the SDK queue.
        CHECK(gateway->HandleEvent(Request(&receiver, gateway.get(), "delivery-queued")));
        PluginManager::Invalidate(*context);
        CHECK(loop.Contains(gateway.get()));
        Drain(); // Simulate another plugin pumping events inside Stop.
        CHECK(requests.size() == 1 && cancelled == 1 && receiver.replies.empty());

        gateway->Stop();
        CHECK(!loop.Contains(gateway.get()));
        gateway.reset(); // Context-owned endpoint must outlive the processor safely.
        Drain();
        CHECK(receiver.replies.size() == 2);
        for (const auto& reply : receiver.replies) {
            CHECK(reply.sender == nullptr && reply.receiver == &receiver);
            CHECK(reply.data.value("json").toString().contains("command processor stopped"));
        }
        CHECK(!provider->Respond(requests.front(), Result()));
    }
    {
        ProcessorEventLoop loop;
        CommandProcessor gateway(loop); // The local-loop unit-test constructor stays active.
        Receiver receiver(loop);
        Stopper stopper(loop, gateway);
        RequestId request = 0;
        auto provider = loop.CreateEndpoint("reply-before-stop", [&](const EventDelivery& event) {
            if (event.kind == EventDelivery::Kind::Request) request = event.requestId;
        });
        CHECK(provider->Provide("command.lifecycle/v1"));
        CHECK(loop.Post(Request(&receiver, &gateway, "completed")));
        Drain();
        CHECK(request != 0 && provider->Respond(request, Result()));
        // Completion queues its reply behind Stop. Unregister must not discard it.
        CHECK(loop.Post({"test.stop.gateway", nullptr, &stopper}));
        Drain();
        CHECK(!loop.Contains(&gateway) && receiver.replies.size() == 1);
        const auto reply = QJsonDocument::fromJson(receiver.replies.front().data.value("json").toString().toUtf8()).object();
        CHECK(reply.value("ok").toBool());
    }
    {
        ProcessorEventLoop loop;
        CommandProcessor gateway(loop);
        Receiver observer(loop);
        auto ipc = std::make_unique<IpcProcessor>(loop, std::make_unique<LAppIPC>(), gateway);
        RequestId request = 0;
        auto provider = loop.CreateEndpoint("ipc-stop-provider", [&](const EventDelivery& event) {
            if (event.kind == EventDelivery::Kind::Request) request = event.requestId;
        });
        CHECK(provider->Provide("command.lifecycle/v1"));
        std::vector<std::string> replies;
        ipc->ExecuteAsync(R"({"command":"lifecycle"})", "pending",
            [&](std::string json) { replies.push_back(std::move(json)); });
        Drain();
        CHECK(request != 0 && replies.empty());
        gateway.Stop();
        Drain();
        CHECK(replies.size() == 1 && replies.front().find("command processor stopped") != std::string::npos);
        CHECK(!provider->Respond(request, Result()));

        ipc->ExecuteAsync(R"({"command":"lifecycle"})", "unsent",
            [&](std::string json) { replies.push_back(std::move(json)); });
        ipc->Disconnected("desktop-session");
        ipc->Stop();
        CHECK(!loop.Contains(ipc.get()));
        CHECK(replies.size() == 2 && replies.back().find("IPC stopped") != std::string::npos);
        ipc->Stop();
        CHECK(replies.size() == 2);
        ipc.reset();
        Drain();
        CHECK(observer.disconnected == std::vector<QString>{QStringLiteral("desktop-session")});
    }
    return 0;
}
