#pragma once
#include <GL/glew.h>
#include <EGL/egl.h>
#include "processor/interaction/window/DesktopWindow.hpp"
#include "DispatchedInput.hpp"
#include <QEventLoop>
#include <QTimer>

// Only the platform boundary is replaced. The real desktop assembler, input
// policy, model loading, commands and render-thread lifecycle remain in use.
class DelegateTestWindow final : public DesktopWindow
{
public:
    explicit DelegateTestWindow(IDesktopWindowEvents** observed = nullptr) : _observed(observed) {}
    std::unique_ptr<ModelRenderTarget> RenderTarget(const std::string&) override
    { return CreateEglModelRenderTarget("auto"); }
    bool UploadFrame(const RenderFrame&) override { return true; }
    bool Initialize(int, int, IDesktopWindowEvents& receiver) override
    {
        _input.Set(receiver);
        if (_observed) *_observed = &_input;
        _display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (_display == EGL_NO_DISPLAY || !eglInitialize(_display, nullptr, nullptr)) return false;
        const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
        EGLConfig config;
        EGLint count = 0;
        if (!eglChooseConfig(_display, attributes, &config, 1, &count) || count != 1 ||
            !eglBindAPI(EGL_OPENGL_API)) return false;
        const EGLint size[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        _surface = eglCreatePbufferSurface(_display, config, size);
        _context = eglCreateContext(_display, config, EGL_NO_CONTEXT, nullptr);
        return _surface != EGL_NO_SURFACE && _context != EGL_NO_CONTEXT && MakeCurrent();
    }
    bool Run(const std::function<bool()>& frame) override
    {
        bool finished = false;
        QEventLoop loop;
        QTimer frames;
        QObject::connect(&frames, &QTimer::timeout, &loop, [&] {
            if (!frame()) { finished = true; loop.quit(); }
        });
        frames.start(1);
        QTimer::singleShot(2000, &loop, &QEventLoop::quit);
        loop.exec();
        return finished;
    }
    bool StartFrames(std::function<bool()> frame, std::function<void(bool)> finished) override
    {
        QObject::connect(&_frames, &QTimer::timeout, &_frames,
            [this, frame = std::move(frame), finished = std::move(finished)] {
                if (!frame()) { _frames.stop(); finished(true); }
            });
        _frames.start(1);
        return true;
    }
    bool MakeCurrent() override { return eglMakeCurrent(_display, _surface, _surface, _context); }
    void Shutdown() override
    {
        _frames.stop();
        if (_display == EGL_NO_DISPLAY) return;
        eglMakeCurrent(_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (_context != EGL_NO_CONTEXT) eglDestroyContext(_display, _context);
        if (_surface != EGL_NO_SURFACE) eglDestroySurface(_display, _surface);
        eglTerminate(_display);
        _context = EGL_NO_CONTEXT; _surface = EGL_NO_SURFACE; _display = EGL_NO_DISPLAY;
    }
    bool Available() const override { return _context != EGL_NO_CONTEXT; }
    int Width() const override { return 16; }
    int Height() const override { return 16; }
    unsigned Framebuffer() const override { return 0; }
    bool SetInputRegion(const std::vector<ModelRegion>&) override { return true; }
    bool KeyboardGrabbed() const override { return _grabbed; }
    ModelKeyboardGrabState::GrabResult SetKeyboardGrab(bool value) override
    { _grabbed = value; return ModelKeyboardGrabState::GrabResult::Changed; }
    bool OutputNeedsRebind() const override { return false; }
    void ReconcileOutputs() override {}
    std::vector<OutputSnapshot> Outputs() const override { return {{1, "test", 0, 0, 16, 16}}; }
    OutputId CurrentOutput() const override { return 1; }
    bool SwitchOutput(OutputId id) override { return id == 1; }
private:
    IDesktopWindowEvents** _observed;
    DispatchedInput _input;
    QTimer _frames;
    EGLDisplay _display = EGL_NO_DISPLAY;
    EGLSurface _surface = EGL_NO_SURFACE;
    EGLContext _context = EGL_NO_CONTEXT;
    bool _grabbed = false;
};
