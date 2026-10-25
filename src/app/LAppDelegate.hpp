#pragma once

#include "processor/ProcessorRuntime.hpp"
#include "contracts/ModelExchange.hpp"
#include <memory>

class ModelProcessor;
class InteractionProcessor;
class InputListener;
class CommandProcessor;
class IpcProcessor;
class SignalProcessor;

// 组装与生命周期入口；不暴露处理器，不拥有领域状态，不转发业务。
class LAppDelegate
{
public:
    static LAppDelegate* GetInstance();
    static void ReleaseInstance();
    bool Initialize();
    bool Release();
    bool Run();
private:
    friend struct ProcessorTestAccess;
    LAppDelegate() = default;
    ~LAppDelegate();
    std::unique_ptr<ProcessorRuntime> _processors;
    std::shared_ptr<ModelExchange> _exchange;
    std::unique_ptr<ModelProcessor> _models;
    std::unique_ptr<InteractionProcessor> _interaction;
    std::unique_ptr<InputListener> _input;
    std::unique_ptr<CommandProcessor> _commands;
    std::unique_ptr<IpcProcessor> _ipcProcessor;
    std::unique_ptr<SignalProcessor> _signals;
};
