#pragma once

#include <cstdint>

// Window events report input; only the application chooses policy.
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
    // Output replacement may pump Qt events while the surface is unavailable.
    virtual void OutputSwitchStarted() {}
    virtual void OutputSwitchFinished() {}
};
