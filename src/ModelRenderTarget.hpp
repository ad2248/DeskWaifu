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
    virtual bool Prepare(QThread*) { return true; }
    virtual bool Initialize() = 0;
    virtual bool MakeCurrent() = 0;
    virtual bool Bind(int width, int height) = 0;
    virtual unsigned Framebuffer() const = 0;
    virtual bool Read(std::vector<unsigned char>& rgba) = 0;
    virtual bool Shutdown() = 0;
};

std::unique_ptr<ModelRenderTarget> CreateEglModelRenderTarget(const std::string& device);
std::unique_ptr<ModelRenderTarget> CreateQtModelRenderTarget();
