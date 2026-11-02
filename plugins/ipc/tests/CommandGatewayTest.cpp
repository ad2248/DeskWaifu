#include "processor/command/CommandProcessor.hpp"
#include <QCoreApplication>
#include <iostream>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::cerr << __LINE__ << ": " << #c << '\n'; return 1; } } while (false)

namespace {
void Drain() { for (int i = 0; i < 20; ++i) QCoreApplication::processEvents(); }

class Caller final : public Processor
{
public:
    using Processor::Processor;
    std::vector<ProcessorEvent> replies;
    bool HandleEvent(const ProcessorEvent& event) override
    {
        if (event.type != "ipc.command.result") return false;
        replies.push_back(event);
        return true;
    }
};

class LegacyObserver final : public Processor
{
public:
    using Processor::Processor;
    int commands = 0, disconnects = 0;
    bool HandleEvent(const ProcessorEvent& event) override
    {
        if (event.type == "interaction.command") ++commands;
        if (event.type == "command.disconnected") ++disconnects;
        return false;
    }
};

ProcessorEventPtr Reply(const QString& json)
{
    return std::make_shared<ProcessorEvent>("command.result", nullptr, nullptr, QVariantMap{{"json", json}});
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ProcessorEventLoop loop;
    CommandProcessor gateway(loop);
    Caller caller(loop);
    LegacyObserver observer(loop);
    const auto send = [&](const QString& json, const QString& connection = "client") {
        loop.Post({"command.request", &caller, &gateway, {{"json", json}, {"connection", connection}}});
        Drain();
    };
    const auto lastJson = [&] { return caller.replies.back().data.value("json").toString(); };

    // No provider is a protocol failure, never a static route into another domain.
    send(R"({"command":"get_status"})");
    CHECK(caller.replies.size() == 1 && lastJson().contains("no provider"));
    CHECK(observer.commands == 0);

    std::vector<EventDelivery> requests;
    int cancellations = 0;
    auto provider = loop.CreateEndpoint("test-command-provider", [&](const EventDelivery& delivery) {
        if (delivery.kind == EventDelivery::Kind::Request) requests.push_back(delivery);
        if (delivery.kind == EventDelivery::Kind::Cancellation) ++cancellations;
    });
    CHECK(provider->Provide("command.custom/v1"));
    CHECK(provider->Provide("command.quit/v1"));
    send(R"({"command":"custom","token":"do-not-log"})", "first");
    send(R"({"command":"custom"})", "second");
    CHECK(requests.size() == 2 && requests[0].requestId != requests[1].requestId);
    CHECK(requests[0].topic == "command.custom/v1");
    CHECK(requests[0].payload->type == "command.invoke");
    CHECK(requests[0].payload->data.value("connection").toString() == "first");
    CHECK(requests[0].payload->data.value("json").toString().contains("do-not-log"));
    CHECK(provider->Respond(requests[1].requestId, Reply(R"({"ok":true,"value":2})")));
    CHECK(provider->Respond(requests[0].requestId, Reply(R"({"ok":true,"value":1})")));
    Drain();
    CHECK(caller.replies.size() == 3);
    CHECK(caller.replies[1].data.value("connection").toString() == "second");
    CHECK(caller.replies[2].data.value("connection").toString() == "first");

    send(R"({"command":"custom"})");
    CHECK(provider->Respond(requests.back().requestId, Reply(R"({"ok":true,"ok":false,"token":"secret"})")));
    Drain();
    CHECK(lastJson().contains("invalid command provider response") && !lastJson().contains("secret"));

    const auto beforeInvalid = requests.size();
    send(R"({"command":"custom","command":"quit"})");
    CHECK(lastJson().contains("Invalid JSON"));
    send(R"({"command":42})");
    CHECK(lastJson().contains("invalid command fields"));
    send(R"({"command":"custom","stream_source":true})", "");
    CHECK(lastJson().contains("stream requires a connection"));
    CHECK(requests.size() == beforeInvalid);

    send(R"({"command":"custom"})", "disconnected");
    const auto abandoned = requests.back().requestId;
    const auto beforeDisconnect = caller.replies.size();
    loop.Post({"command.disconnected", &caller, nullptr, {{"connection", "disconnected"}}});
    Drain();
    CHECK(observer.disconnects == 1 && cancellations == 1);
    CHECK(!provider->Respond(abandoned, Reply(R"({"ok":true})")));
    CHECK(caller.replies.size() == beforeDisconnect);

    auto duplicate = loop.CreateEndpoint("duplicate-command-provider", [](const EventDelivery&) {});
    CHECK(duplicate->Provide("command.custom/v1"));
    send(R"({"command":"custom"})");
    CHECK(lastJson().contains("ambiguous provider"));
    duplicate->Invalidate();

    send(R"({"command":"quit"})");
    CHECK(provider->Respond(requests.back().requestId, Reply(R"({"ok":true})")));
    Drain();
    CHECK(caller.replies.back().data.value("quit").toBool());

    send(R"({"command":"custom"})", "stopped");
    const auto stoppedRequest = requests.back().requestId;
    const auto beforeStop = caller.replies.size();
    gateway.Stop();
    Drain();
    CHECK(caller.replies.size() == beforeStop + 1 && lastJson().contains("stopped"));
    CHECK(!provider->Respond(stoppedRequest, Reply(R"({"ok":true})")));
    Drain();
    CHECK(caller.replies.size() == beforeStop + 1);
    return 0;
}
