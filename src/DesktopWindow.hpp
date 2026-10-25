#pragma once

#include "ModelKeyboardGrab.hpp"
#include "ModelRegion.hpp"
#include "OutputController.hpp"
#include "DesktopWindowEvents.hpp"
#include <functional>
#include <memory>
#include "ModelRenderTarget.hpp"
struct RenderFrame;

// 向应用暴露的坐标为输出设备局部逻辑像素。
// 后端持有表面、GL 上下文并负责呈现，不持有模型资源。
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
    virtual bool UploadFrame(const RenderFrame&) { return false; }
    virtual std::unique_ptr<ModelRenderTarget> RenderTarget(const std::string& device)
    { return device.empty() || device == "auto" ? CreateQtModelRenderTarget() : CreateEglModelRenderTarget(device); }
    virtual bool SetInputRegion(const std::vector<ModelRegion>& regions) = 0;
    virtual bool KeyboardGrabbed() const = 0;
    virtual ModelKeyboardGrabState::GrabResult SetKeyboardGrab(bool grabbed) = 0;
    virtual bool OutputNeedsRebind() const = 0;
    virtual void ReconcileOutputs() = 0;
};

std::unique_ptr<DesktopWindow> CreateDesktopWindow();
