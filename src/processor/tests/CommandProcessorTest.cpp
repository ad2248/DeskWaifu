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
        bool HandleEvent(const ProcessorEvent& event) override
        {
            if (event.type != "ipc.command.result") return false;
            replies.push_back(event.data.value("json").toString().toStdString()); return true;
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
    return 0;
}
