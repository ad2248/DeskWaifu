#pragma once

#include "event/ProcessorEventLoop.hpp"
#include "processor/model/ModelExchange.hpp"
#include <memory>

class ModelProcessor;
class InteractionProcessor;
class InputListener;
class SignalProcessor;

// 组装与生命周期入口；不暴露处理器，不拥有领域状态，不转发业务。
class LAppDelegate
{
public:
    // 延迟创建进程级实例；返回借用指针，由 ReleaseInstance() 销毁。
    static LAppDelegate* GetInstance();
    static void ReleaseInstance();
    // 创建自有事件循环并初始化处理器；任一必要组件启动失败时返回 false。
    bool Initialize();
    // 借用 loop，调用方须保证它存活至 Release() 完成；在 Qt 应用线程调用。
    bool Initialize(ProcessorEventLoop& loop);
    // 启动异步呈现；返回是否成功启动，结束时通过 finished 报告呈现结果。
    bool StartFrames(std::function<void(bool)> finished);
    // 先停止输入源和模型线程，再拆除窗口；GPU 清理失败时返回 false。
    bool Release();
    // 阻塞运行呈现循环，结束后释放资源并销毁单例；返回运行与清理是否均成功。
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
