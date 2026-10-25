#include "ModelRenderTarget.hpp"
#include "OffscreenRenderer.hpp"
#include "ReadRgbaFrame.hpp"
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QThread>
#include <iostream>

namespace {
class EglTarget final : public ModelRenderTarget
{
public:
    explicit EglTarget(std::string device) : _device(std::move(device)) {}
    bool Initialize() override { _renderer.reset(new OffscreenRenderer); return _renderer->Initialize(_device); }
    bool MakeCurrent() override { return _renderer && _renderer->MakeCurrent(); }
    bool Bind(int w, int h) override { return _renderer && _renderer->BindFrame(w, h); }
    unsigned Framebuffer() const override { return _renderer ? _renderer->Framebuffer() : 0; }
    bool Read(std::vector<unsigned char>& rgba) override { return _renderer && _renderer->ReadFrame(rgba); }
    bool Shutdown() override
    {
        if (!_renderer) return true;
        if (!_renderer->Shutdown()) {
            (void)_renderer.release(); // 不在 GUI 线程重试 GL 资源析构。
            return false;
        }
        _renderer.reset();
        return true;
    }
private:
    std::string _device;
    std::unique_ptr<OffscreenRenderer> _renderer;
};

class QtTarget final : public ModelRenderTarget
{
public:
    QtTarget()
    {
        Q_ASSERT(QThread::currentThread() == qGuiApp->thread());
        QSurfaceFormat format;
        format.setRenderableType(QSurfaceFormat::OpenGL);
        format.setVersion(3, 3);
        format.setProfile(QSurfaceFormat::CompatibilityProfile);
        format.setOption(QSurfaceFormat::DeprecatedFunctions);
        format.setAlphaBufferSize(8); format.setDepthBufferSize(24); format.setStencilBufferSize(8);
        _surface.reset(new QOffscreenSurface);
        _surface->setFormat(format); _surface->create();
        _context.reset(new QOpenGLContext);
        _context->setFormat(_surface->format());
    }
    bool Prepare(QThread* worker) override
    {
        if (!QOpenGLContext::supportsThreadedOpenGL() || !_surface->isValid() || !_context->create()) return false;
        _context->moveToThread(worker);
        return true;
    }
    bool Initialize() override
    {
        if (!_context->makeCurrent(_surface.get()) || _context->isOpenGLES() ||
            _context->format().profile() == QSurfaceFormat::CoreProfile || _context->format().version() < qMakePair(3, 3)) return false;
        if (!InitializeRenderGl()) return false;
        auto* gl = _context->extraFunctions();
        std::cout << "[GPU] Render thread: " << gl->glGetString(GL_VENDOR) << " / " << gl->glGetString(GL_RENDERER) << std::endl;
        return true;
    }
    bool MakeCurrent() override { return _context && _context->makeCurrent(_surface.get()); }
    bool Bind(int width, int height) override
    {
        if (!_frame || _frame->width() != width || _frame->height() != height) {
            GLint maxSize = 0; _context->extraFunctions()->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
            if (width <= 0 || height <= 0 || width > maxSize || height > maxSize) return false;
            _frame.reset(new QOpenGLFramebufferObject(width, height, QOpenGLFramebufferObject::CombinedDepthStencil));
        }
        return _frame->isValid() && _frame->bind();
    }
    unsigned Framebuffer() const override { return _frame ? _frame->handle() : 0; }
    bool Read(std::vector<unsigned char>& rgba) override
    { return _frame && ReadRgbaFrame(_frame->handle(), _frame->width(), _frame->height(), rgba); }
    bool Shutdown() override
    {
        if (!_context) return true;
        if (!_context->makeCurrent(_surface.get())) {
            (void)_frame.release(); _context.reset(); return false;
        }
        _frame.reset();
        _context->doneCurrent(); _context.reset();
        return true;
    }
private:
    std::unique_ptr<QOffscreenSurface> _surface; // 在 GUI 线程创建和销毁。
    std::unique_ptr<QOpenGLContext> _context;   // 移入渲染线程，并在那里销毁。
    std::unique_ptr<QOpenGLFramebufferObject> _frame;
};
}

std::unique_ptr<ModelRenderTarget> CreateEglModelRenderTarget(const std::string& device)
{ return std::unique_ptr<ModelRenderTarget>(new EglTarget(device)); }
std::unique_ptr<ModelRenderTarget> CreateQtModelRenderTarget()
{ return std::unique_ptr<ModelRenderTarget>(new QtTarget); }
