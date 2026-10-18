#pragma once

#include "ModelKeyboardGrab.hpp"
#include "ModelRegion.hpp"
#include "OutputController.hpp"
#include "DesktopWindowEvents.hpp"
#include <functional>
#include <memory>

// Coordinates exposed to the application are output-local logical pixels.
// The backend owns the surface, GL context and presentation, never model resources.
class DesktopWindow : public IOutputBackend
{
public:
    virtual bool Initialize(int width, int height, IDesktopWindowEvents& events) = 0;
    virtual bool Run(const std::function<bool()>& frame) = 0;
    virtual bool MakeCurrent() = 0;
    virtual void Shutdown() = 0;
    virtual bool Available() const = 0;
    virtual int Width() const = 0;
    virtual int Height() const = 0;
    virtual unsigned int Framebuffer() const = 0;
    virtual bool SetInputRegion(const std::vector<ModelRegion>& regions) = 0;
    virtual bool KeyboardGrabbed() const = 0;
    virtual ModelKeyboardGrabState::GrabResult SetKeyboardGrab(bool grabbed) = 0;
    virtual bool OutputNeedsRebind() const = 0;
    virtual void ReconcileOutputs() = 0;
};

std::unique_ptr<DesktopWindow> CreateDesktopWindow();
