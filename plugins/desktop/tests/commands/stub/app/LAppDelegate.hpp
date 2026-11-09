#pragma once

#include "processor/Processor.hpp"
#include <memory>

// Replace only the desktop runtime boundary, not DesktopCommands or its entry.
// 命令测试运行时替身：保留正式命令入口，以事件边界模拟模型和交互回复。
class LAppDelegate
{
public:
    static LAppDelegate* GetInstance();
    static void ReleaseInstance();
    // 借用事件循环并注册模拟边界；启动探测失败返回 false，loop 须存活更久。
    bool Initialize(ProcessorEventLoop& loop);
    // 仅执行启动阶段探测，不产生真实帧，也不调用传入完成回调。
    bool StartFrames(std::function<void(bool)>);
    // 停止模拟边界的事件接收；对象由 ReleaseInstance 解除持有。
    bool Release();
private:
    ProcessorEventLoop* _loop = nullptr;
    std::unique_ptr<Processor> _boundary;
};
