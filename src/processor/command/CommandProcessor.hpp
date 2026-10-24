#pragma once

#include "processor/Processor.hpp"
#include "ICommandService.hpp"
#include <memory>

struct PendingCommand
{
    std::string json, connection;
    std::function<void(std::string)> complete;
};
using PendingCommandPtr = std::shared_ptr<PendingCommand>;
Q_DECLARE_METATYPE(PendingCommandPtr)

// Routes existing configuration/control commands through the same ordered
// event dispatcher. Platform/config state stays on the GUI thread; the model
// adapter forwards only model operations to its worker.
class CommandProcessor final : public Processor, public ICommandService
{
public:
    CommandProcessor(ProcessorEventLoop& loop, ICommandService& commands) : Processor(loop), _commands(commands) {}
    std::string Execute(const std::string&, const std::string&) override
    { return R"({"ok":false,"error":"use asynchronous command dispatch"})"; }
    void ExecuteAsync(const std::string& json, const std::string& connection,
        std::function<void(std::string)> complete) override
    {
        auto command = std::make_shared<PendingCommand>(PendingCommand{json, connection, std::move(complete)});
        if (!Send(QStringLiteral("command.execute"), this, {{"command", QVariant::fromValue(command)}}))
            command->complete(R"({"ok":false,"error":"command processor stopped"})");
    }
    void Disconnected(const std::string& connection) override
    { Send(QStringLiteral("command.disconnected"), this, {{"connection", QString::fromStdString(connection)}}); }
    bool HandleEvent(const ProcessorEvent& event) override
    {
        if (event.type == QStringLiteral("command.execute")) {
            const auto command = event.data.value("command").value<PendingCommandPtr>();
            if (command) _commands.ExecuteAsync(command->json, command->connection, command->complete);
            return true;
        }
        if (event.type == QStringLiteral("command.disconnected")) {
            _commands.Disconnected(event.data.value("connection").toString().toStdString());
            return true;
        }
        return false;
    }
private:
    ICommandService& _commands;
};
