#pragma once

#include "event/ProcessorEventLoop.hpp"
#include "contracts/ModelExchange.hpp"
#include <memory>

class ModelProcessor;
class InteractionProcessor;
class InputListener;
class SignalProcessor;

// 组装与生命周期入口；不暴露处理器，不拥有领域状态，不转发业务。
class LAppDelegate
{
public:
    static LAppDelegate* GetInstance();
    static void ReleaseInstance();
    bool Initialize();
    bool Initialize(ProcessorEventLoop& loop);
    bool StartFrames(std::function<void(bool)> finished);
    bool Release();
    bool Run();
private:
    friend struct ProcessorTestAccess;
    LAppDelegate() = default;
    ~LAppDelegate();
    std::unique_ptr<ProcessorEventLoop> _ownedLoop;
    std::shared_ptr<ModelExchange> _exchange;
    std::unique_ptr<ModelProcessor> _models;
    std::unique_ptr<InteractionProcessor> _interaction;
    std::unique_ptr<InputListener> _input;
    std::unique_ptr<SignalProcessor> _signals;
};
