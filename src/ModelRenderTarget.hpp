#pragma once

#include <memory>
#include <string>
#include <vector>

class QThread;

// Construct/prepare on the GUI thread; all GL methods, initialization and shutdown
// run on the render thread. Destruction follows thread join on the GUI thread.
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
