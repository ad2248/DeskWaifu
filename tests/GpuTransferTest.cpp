#include <GL/glew.h>
#include <EGL/egl.h>
#include "DesktopWindow.hpp"
#include <QGuiApplication>
#include <QWindow>
#include <cstdlib>
#include <dlfcn.h>
#include <iostream>

namespace {
void Require(bool ok, const char* message)
{
    if (!ok) { std::cerr << message << '\n'; std::abort(); }
}
struct Events : IDesktopWindowEvents
{
    void PointerEntered(double, double) override {}
    void PointerLeft() override {}
    void PointerMoved(double, double) override {}
    void PointerButton(int, bool) override {}
    void PointerScroll(double) override {}
    void KeyboardChanged() override {}
    void KeyboardKey(uint32_t, bool) override {}
    void SurfaceClosed() override {}
};
QWindow* surface = nullptr;
int swaps = 0;
int renderedWidth = 0, renderedHeight = 0;
}

// Verify pixels on the presentation GPU, not merely in the render GPU's FBO.
extern "C" EGLBoolean eglSwapBuffers(EGLDisplay display, EGLSurface window)
{
    if (surface) {
        const auto scale = surface->devicePixelRatio();
        const double scaleX = scale * surface->width() / renderedWidth;
        const double scaleY = scale * surface->height() / renderedHeight;
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glReadBuffer(GL_BACK);
        unsigned char pixel[4] = {};
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glReadPixels(qRound(25 * scaleX), qRound(25 * scaleY), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        Require(pixel[0] == 32 && pixel[1] == 64 && pixel[2] == 128 && pixel[3] == 128,
            "Cross-GPU transfer changed premultiplied RGBA pixels");
        glReadPixels(qRound(25 * scaleX), qRound((renderedHeight - 25) * scaleY), 1, 1,
            GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        Require(pixel[3] == 0, "Transferred frame was flipped or background became opaque");
        Require(glGetError() == GL_NO_ERROR, "Presentation GPU GL error");
        ++swaps;
    }
    static const auto swap = reinterpret_cast<decltype(&eglSwapBuffers)>(dlsym(RTLD_NEXT, "eglSwapBuffers"));
    Require(swap, "Missing real EGL swap");
    return swap(display, window);
}

int main(int argc, char** argv)
{
    const char* device = getenv("DESKWAIFU_TEST_RENDER_DEVICE");
    if (!device) return 77;
    qputenv("DESKWAIFU_RENDER_DEVICE", device);
    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    Events events;
    auto window = CreateDesktopWindow();
    Require(window->Initialize(640, 480, events), "Cannot initialize cross-GPU window");
    surface = dynamic_cast<QWindow*>(window.get());
    const auto outputs = window->Outputs();
    Require(window->SetInputRegion({{20, 20, 40, 40}}), "Cannot set model input region with offscreen context");
    int frames = 0;
    Require(window->Run([&] {
        Require(window->Framebuffer() != 0, "Missing offscreen framebuffer");
        renderedWidth = window->Width();
        renderedHeight = window->Height();
        if (++frames == 5) return false;
        if (outputs.size() > 1 && frames == 2) {
            Require(window->SwitchOutput(outputs[1].id), "Cross-GPU output switch failed");
            Require(window->MakeCurrent(), "Cannot restore render context after output switch");
            glBindFramebuffer(GL_FRAMEBUFFER, window->Framebuffer());
        }
        if (outputs.size() > 1 && frames == 3) {
            Require(window->SwitchOutput(outputs[0].id), "Cross-GPU switch back failed");
            Require(window->MakeCurrent(), "Cannot restore render context after switching back");
            glBindFramebuffer(GL_FRAMEBUFFER, window->Framebuffer());
        }
        // Reproduce a surface size change after the model FBO was bound. The
        // upload must retain the source stride, then scale to the new surface.
        if (frames == 4) surface->resize(surface->width() / 2, surface->height() / 2);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_SCISSOR_TEST);
        glScissor(20, 20, 40, 40);
        glClearColor(.125f, .25f, .5f, .5f);
        glClear(GL_COLOR_BUFFER_BIT);
        // Leave scissoring enabled to ensure presentation copies the full frame.
        return true;
    }), "Cross-GPU frame loop failed");
    Require(swaps == 4, "Missing transferred frames");
    surface = nullptr;
    window->Shutdown();
    window->Shutdown();
}
