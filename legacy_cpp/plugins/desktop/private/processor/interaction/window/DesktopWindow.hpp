#pragma once

#include "DesktopWindowEvents.hpp"
#include "ModelKeyboardGrab.hpp"
#include "OutputController.hpp"
#include "contracts/ModelRegion.hpp"
#include "render/ModelRenderTarget.hpp"
#include <functional>
#include <memory>
struct RenderFrame;

// 向应用暴露的坐标为输出设备局部逻辑像素。
// 后端持有表面、GL 上下文并负责呈现，不持有模型资源。
class DesktopWindow : public IOutputBackend {
public:
    // 在 Qt 应用线程创建表面与呈现上下文，失败返回 false。
    // 借用 events 至 Shutdown()；width、height 是请求尺寸，以实际尺寸为准。
    virtual bool Initialize(int width, int height, IDesktopWindowEvents& events) = 0;
    // 阻塞驱动呈现；frame 返回 false 表示正常结束，后端呈现失败返回 false。
    virtual bool Run(const std::function<bool()>& frame) = 0;
    // 非阻塞驱动呈现；成功启动返回 true，结束时 finished 接收后端结果。
    // 默认实现不支持；回调及窗口操作均留在 Qt 应用线程。
    virtual bool StartFrames(std::function<bool()> frame, std::function<void(bool)> finished) { return false; }
    // 在调用线程绑定窗口呈现上下文；绑定失败返回 false。
    virtual bool MakeCurrent() = 0;
    // 停止呈现并释放表面资源；调用前应已回收模型持有的 GPU 资源。
    virtual void Shutdown() = 0;
    virtual bool Available() const = 0;
    virtual int Width() const = 0;
    virtual int Height() const = 0;
    // 返回当前呈现帧缓冲句柄；尚未创建时为 0，不转移 GL 对象所有权。
    virtual unsigned int Framebuffer() const = 0;
    // 在当前呈现上下文中上传像素帧；格式、尺寸或 GL 操作失败返回 false。
    virtual bool UploadFrame(const RenderFrame&) { return false; }
    virtual std::unique_ptr<ModelRenderTarget> RenderTarget(const std::string& device)
    {
        return device.empty() || device == "auto" ? CreateQtModelRenderTarget() : CreateEglModelRenderTarget(device);
    }
    // 设置局部逻辑像素输入区域，空列表表示全部穿透；窗口不可用时返回 false。
    virtual bool SetInputRegion(const std::vector<ModelRegion>& regions) = 0;
    virtual bool KeyboardGrabbed() const = 0;
    // 请求或释放键盘交互；返回变更、未变或拒绝，不保证合成器已授予焦点。
    virtual ModelKeyboardGrabState::GrabResult SetKeyboardGrab(bool grabbed) = 0;
    virtual bool OutputNeedsRebind() const = 0;
    // 当前输出失效时尝试重绑定；无法恢复时通过窗口事件通知关闭。
    virtual void ReconcileOutputs() = 0;
};

std::unique_ptr<DesktopWindow> CreateDesktopWindow();
