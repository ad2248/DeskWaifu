#include "processor/command/CommandProcessor.hpp"
#include <QCoreApplication>
#include <iostream>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::cerr << __LINE__ << ": " << #c << '\n'; return 1; } } while (false)

class Domain final : public Processor
{
public:
    using Processor::Processor;
    std::vector<std::string> calls;
    IProcessor* requester = nullptr;
    bool HandleEvent(const ProcessorEvent& event) override
    {
        if (event.type != "interaction.command") return false;
        calls.push_back(event.data.value("connection").toString().toStdString());
        requester = event.sender;
        return true;
    }
    void Complete(const QString& reply) { Send("interaction.result", requester, {{"json", reply}}); }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ProcessorEventLoop loop; Domain domain(loop);
    class Caller final : public Processor
    {
    public:
        using Processor::Processor;
        std::vector<std::string> replies;
        std::vector<std::string> connections;
        bool HandleEvent(const ProcessorEvent& event) override
        {
            if (event.type != "ipc.command.result") return false;
            replies.push_back(event.data.value("json").toString().toStdString());
            connections.push_back(event.data.value("connection").toString().toStdString()); return true;
        }
    } caller(loop);
    auto& replies = caller.replies;
    {
        CommandProcessor processor(loop);
        loop.Post({"command.request", &caller, &processor, {{"json", R"({"command":"get_status"})"}, {"connection", "one"}}});
        loop.Post({"command.request", &caller, &processor, {{"json", R"({"command":"get_status"})"}, {"connection", "two"}}});
        CHECK(domain.calls.empty() && replies.empty());
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();
        CHECK(domain.calls == std::vector<std::string>{"one"} && replies.empty());
        domain.Complete("first");
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();
        CHECK(domain.calls == std::vector<std::string>({"one", "two"}));
        CHECK(replies == std::vector<std::string>{"first"});
        domain.Complete("second");
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();
        CHECK(replies == std::vector<std::string>({"first", "second"}));
        loop.Post({"command.request", &caller, &processor, {{"json", R"({"command":"get_status"})"}, {"connection", "cancelled"}}});
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();
        processor.Stop();
        for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();
        CHECK(replies.size() == 3 && replies.back().find("stopped") != std::string::npos);
    }
    domain.Complete("late");
    for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();
    CHECK(replies.size() == 3);
    {
        CommandProcessor processor(loop);
        std::vector<RequestId> requests;
        auto provider = loop.CreateEndpoint("test-plugin-commands", [&](const EventDelivery& delivery) {
            if (delivery.kind == EventDelivery::Kind::Request) requests.push_back(delivery.requestId);
        });
        CHECK(provider->Provide("command.custom_plugin_action/v1"));
        const auto send = [&](const char* connection) {
            loop.Post({"command.request", &caller, &processor,
                {{"json", R"({"command":"custom_plugin_action"})"}, {"connection", connection}}});
        };
        send("first-plugin-client"); send("second-plugin-client");
        for (int i = 0; i < 20; ++i) QCoreApplication::processEvents();
        CHECK(requests.size() == 2 && requests[0] != requests[1]);
        auto reply = [](const char* json) {
            return std::make_shared<ProcessorEvent>("command.result", nullptr, nullptr, QVariantMap{{"json", json}});
        };
        CHECK(provider->Respond(requests[1], reply(R"({"ok":true,"value":2})")));
        CHECK(provider->Respond(requests[0], reply(R"({"ok":true,"value":1})")));
        for (int i = 0; i < 20; ++i) QCoreApplication::processEvents();
        CHECK(caller.connections[3] == "second-plugin-client" && caller.connections[4] == "first-plugin-client");
        CHECK(replies[3].find("2") != std::string::npos && replies[4].find("1") != std::string::npos);
        auto duplicate = loop.CreateEndpoint("duplicate-plugin", [](const EventDelivery&) {});
        CHECK(duplicate->Provide("command.custom_plugin_action/v1"));
        send("ambiguous");
        for (int i = 0; i < 20; ++i) QCoreApplication::processEvents();
        CHECK(replies.size() == 6 && replies.back().find("false") != std::string::npos && requests.size() == 2);
        duplicate->Invalidate();
        send("provider-stopped");
        for (int i = 0; i < 20; ++i) QCoreApplication::processEvents();
        CHECK(requests.size() == 3);
        provider->Invalidate();
        for (int i = 0; i < 20; ++i) QCoreApplication::processEvents();
        CHECK(replies.size() == 7 && replies.back().find("false") != std::string::npos);
    }
    return 0;
}
