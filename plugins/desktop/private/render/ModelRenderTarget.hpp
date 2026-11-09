#pragma once

#include <memory>
#include <string>
#include <vector>

class QThread;

// 在 GUI 线程构造和准备；所有 GL 方法、初始化和关闭操作均在渲染线程执行。
// 等待渲染线程结束后，在 GUI 线程销毁此对象。
class ModelRenderTarget
{
public:
    virtual ~ModelRenderTarget() = default;
    // 在 GUI 线程准备上下文并转移至指定工作线程；失败返回 false。
    virtual bool Prepare(QThread*) { return true; }
    // 在工作线程初始化 GL，并使上下文成为当前上下文；失败返回 false。
    virtual bool Initialize() = 0;
    // 尝试在调用线程绑定本目标上下文，成功返回 true。
    virtual bool MakeCurrent() = 0;
    // 创建或调整并绑定指定尺寸的帧缓冲；上下文须已就绪，失败返回 false。
    virtual bool Bind(int width, int height) = 0;
    // 返回借用的帧缓冲 ID，尚未创建时为 0，不得跨线程使用。
    virtual unsigned Framebuffer() const = 0;
    // 输出紧密排列、自下而上的 RGBA8 数据；读回失败返回 false。
    virtual bool Read(std::vector<unsigned char>& rgba) = 0;
    // 在工作线程释放 GL 资源及上下文，无法安全收尾时返回 false。
    virtual bool Shutdown() = 0;
};

// 创建延迟初始化的 EGL 目标，device 为 PCI 地址或 auto；调用方获得所有权。
std::unique_ptr<ModelRenderTarget> CreateEglModelRenderTarget(const std::string& device);
// 在 GUI 线程创建 Qt 离屏表面；调用方获得目标所有权。
std::unique_ptr<ModelRenderTarget> CreateQtModelRenderTarget();
