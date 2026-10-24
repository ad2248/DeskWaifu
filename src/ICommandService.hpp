#pragma once

#include <string>
#include <functional>

// Transport-independent command port. The transport provides a connection
// identity and reports disconnection; application commands decide whether
// that identity owns an opt-in transient control source.
// Only the transport borrows this service. Commands, disconnect cleanup and
// service destruction must never borrow a transport port back.
class ICommandService
{
public:
    virtual ~ICommandService() = default;
    virtual std::string Execute(const std::string& json, const std::string& connection) = 0;
    // Completion runs on the transport's owning thread, at most once. The
    // callback owns no transport; it may outlive a disconnected connection.
    virtual void ExecuteAsync(const std::string& json, const std::string& connection,
        std::function<void(std::string)> complete) { complete(Execute(json, connection)); }
    virtual void Disconnected(const std::string& connection) = 0;
};
