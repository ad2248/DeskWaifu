#pragma once

#include <string>

class LAppDelegate;
class LAppLive2DManager;

// Transport-independent command port. #78 will route these operations
// through ApplicationService; #79 will transfer external-control ownership.
class ICommandService
{
public:
    virtual ~ICommandService() = default;
    virtual std::string Execute(const std::string& json) = 0;
};

class IExternalControlPort
{
public:
    virtual ~IExternalControlPort() = default;
    virtual void SetMouth(float value) = 0;
    virtual void SetLook(float x, float y) = 0;
    virtual void ResetLook() = 0;
};

class AppCommandHandler : public ICommandService
{
public:
    AppCommandHandler(LAppDelegate& app, LAppLive2DManager& models, IExternalControlPort& control)
        : _app(app), _models(models), _control(control) {}
    std::string Execute(const std::string& json) override;

private:
    LAppDelegate& _app;
    LAppLive2DManager& _models;
    IExternalControlPort& _control;
};
