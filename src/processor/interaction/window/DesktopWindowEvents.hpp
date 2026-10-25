#pragma once

#include <cstdint>

// 窗口事件只报告输入，处理策略由应用决定。
class IDesktopWindowEvents
{
public:
    virtual ~IDesktopWindowEvents() = default;
    virtual void PointerEntered(double x, double y) = 0;
    virtual void PointerLeft() = 0;
    virtual void PointerMoved(double x, double y) = 0;
    virtual void PointerButton(int button, bool pressed) = 0;
    virtual void PointerScroll(double vertical) = 0;
    virtual void KeyboardChanged() = 0;
    virtual void KeyboardKey(std::uint32_t code, bool pressed) = 0;
    virtual void SurfaceClosed() = 0;
    // 替换输出设备时，表面不可用期间仍可能处理 Qt 事件。
    virtual void OutputSwitchStarted() {}
    virtual void OutputSwitchFinished() {}
};
