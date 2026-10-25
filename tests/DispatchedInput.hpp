#pragma once
#include "DesktopWindowEvents.hpp"
#include <QCoreApplication>

// 测试驱动在每次合成输入后让 Qt 派发队列前进，模拟真实窗口事件循环。
// 窗口生命周期回调保持同步，模型操作仍由各测试显式等待。
class DispatchedInput final : public IDesktopWindowEvents
{
public:
    void Set(IDesktopWindowEvents& sink) { _sink = &sink; }
    void PointerEntered(double x, double y) override { _sink->PointerEntered(x, y); Drain(); }
    void PointerLeft() override { _sink->PointerLeft(); Drain(); }
    void PointerMoved(double x, double y) override { _sink->PointerMoved(x, y); Drain(); }
    void PointerButton(int button, bool pressed) override { _sink->PointerButton(button, pressed); Drain(); }
    void PointerScroll(double value) override { _sink->PointerScroll(value); Drain(); }
    void KeyboardChanged() override { _sink->KeyboardChanged(); Drain(); }
    void KeyboardKey(uint32_t code, bool pressed) override { _sink->KeyboardKey(code, pressed); Drain(); }
    void SurfaceClosed() override { _sink->SurfaceClosed(); }
    void OutputSwitchStarted() override { _sink->OutputSwitchStarted(); }
    void OutputSwitchFinished() override { _sink->OutputSwitchFinished(); }
private:
    static void Drain() { for (int i = 0; i < 8; ++i) QCoreApplication::processEvents(); }
    IDesktopWindowEvents* _sink = nullptr;
};
