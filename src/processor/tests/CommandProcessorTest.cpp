#include "processor/command/CommandProcessor.hpp"
#include <QCoreApplication>
#include <iostream>
#include <vector>

#define CHECK(c) do { if (!(c)) { std::cerr << __LINE__ << ": " << #c << '\n'; return 1; } } while (false)

struct Commands : ICommandService
{
    std::vector<std::string> calls;
    std::string Execute(const std::string& json, const std::string& connection) override
    { calls.push_back(connection + ":" + json); return json; }
    void Disconnected(const std::string& connection) override { calls.push_back(connection + ":disconnected"); }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ProcessorEventLoop loop;
    Commands service;
    std::vector<std::string> replies;
    {
        CommandProcessor processor(loop, service);
        processor.ExecuteAsync("one", "client", [&](std::string reply) { replies.push_back(reply); });
        processor.ExecuteAsync("two", "client", [&](std::string reply) { replies.push_back(reply); });
        processor.Disconnected("client");
        CHECK(service.calls.empty() && replies.empty()); // 不能绕过有序事件派发。
        for (int i = 0; i < 8; ++i) QCoreApplication::processEvents();
        CHECK(service.calls == std::vector<std::string>({"client:one", "client:two", "client:disconnected"}));
        CHECK(replies == std::vector<std::string>({"one", "two"}));
        CHECK(!processor.HandleEvent({"unrelated"}));
        processor.ExecuteAsync("must be purged", "client", [&](std::string reply) { replies.push_back(reply); });
    }
    for (int i = 0; i < 4; ++i) QCoreApplication::processEvents();
    CHECK(service.calls.size() == 3 && replies.size() == 2);
    return 0;
}
