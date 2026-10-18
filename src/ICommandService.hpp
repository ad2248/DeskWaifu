#pragma once

#include <string>

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
    virtual void Disconnected(const std::string& connection) = 0;
};
