#pragma once

#include <string>

class LAppDelegate;
class LAppLive2DManager;

// Transport-independent command port. The transport provides a connection
// identity and reports disconnection; application commands decide whether
// that identity owns an opt-in transient control source.
class ICommandService
{
public:
    virtual ~ICommandService() = default;
    virtual std::string Execute(const std::string& json, const std::string& connection) = 0;
    virtual void Disconnected(const std::string& connection) = 0;
};

class AppCommandHandler : public ICommandService
{
public:
    AppCommandHandler(LAppDelegate& app, LAppLive2DManager& models)
        : _app(app), _models(models) {}
    std::string Execute(const std::string& json, const std::string& connection) override;
    void Disconnected(const std::string& connection) override;

private:
    LAppDelegate& _app;
    LAppLive2DManager& _models;
};
