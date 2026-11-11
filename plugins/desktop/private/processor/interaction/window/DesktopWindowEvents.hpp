#pragma once

#include <cstdint>

// 窗口事件只报告输入，处理策略由应用决定。
class IDesktopWindowEvents {
public:
    virtual ~IDesktopWindowEvents() = default;
    // 指针位置使用当前输出的局部逻辑像素；回调在 Qt 应用线程执行。
    virtual void PointerEntered(double x, double y) = 0;
    virtual void PointerLeft() = 0;
    virtual void PointerMoved(double x, double y) = 0;
    // button 为应用约定的左、右、中键编号 0、1、2。
    virtual void PointerButton(int button, bool pressed) = 0;
    // 报告归一化的纵向滚动量，而非原始 Qt 角度单位。
    virtual void PointerScroll(double vertical) = 0;
    // 键盘捕获或设备状态变化时通知调用方清空按键状态。
    virtual void KeyboardChanged() = 0;
    // code 为 evdev 键码，不依赖当前键盘布局。
    virtual void KeyboardKey(std::uint32_t code, bool pressed) = 0;
    virtual void SurfaceClosed() = 0;
    // 替换输出设备时，表面不可用期间仍可能处理 Qt 事件。
    virtual void OutputSwitchStarted() { }
    virtual void OutputSwitchFinished() { }
};
