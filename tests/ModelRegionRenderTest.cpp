#include <GL/glew.h>
#include <EGL/egl.h>
#include "LAppWaylandRegion.hpp"
#include <iostream>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "Failed at line " << __LINE__ << ": " << #condition << '\n'; return 1; } } while (0)

int main()
{
    const EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) return 77;
    const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    CHECK(eglChooseConfig(display, attributes, &config, 1, &count) && count == 1);
    CHECK(eglBindAPI(EGL_OPENGL_API));
    const EGLint size[] = {EGL_WIDTH, 13, EGL_HEIGHT, 9, EGL_NONE};
    const EGLSurface surface = eglCreatePbufferSurface(display, config, size);
    const EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    CHECK(surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT);
    CHECK(eglMakeCurrent(display, surface, surface, context));

    for (const auto requested : {ModelRegion{3, 2, 5, 4}, ModelRegion{},
        ModelRegion{0, 0, 13, 9}, ModelRegion{-3, -2, 8, 6}})
    {
        const auto clip = ResolveModelRegion(true, requested, 13, 9);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(1, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        // Clipping must ignore these incoming states, then restore them.
        glEnable(GL_SCISSOR_TEST);
        glScissor(1, 2, 3, 4);
        glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE);
        glClearColor(0.25f, 0.5f, 0.75f, 1);
        ClipWaylandModelRegion(clip, 13, 9);
        CHECK(glIsEnabled(GL_SCISSOR_TEST));
        GLint scissor[4];
        GLboolean mask[4];
        GLfloat color[4];
        glGetIntegerv(GL_SCISSOR_BOX, scissor);
        glGetBooleanv(GL_COLOR_WRITEMASK, mask);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, color);
        CHECK(scissor[0] == 1 && scissor[1] == 2 && scissor[2] == 3 && scissor[3] == 4);
        CHECK(!mask[0] && mask[1] && !mask[2] && !mask[3]);
        CHECK(color[0] == 0.25f && color[1] == 0.5f && color[2] == 0.75f && color[3] == 1);
        std::vector<unsigned char> pixels(13 * 9 * 4);
        glReadPixels(0, 0, 13, 9, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        for (int y = 0; y < 9; ++y) for (int x = 0; x < 13; ++x)
        {
            const int topY = 8 - y;
            const bool inside = x >= clip.x && x < clip.x + clip.width
                && topY >= clip.y && topY < clip.y + clip.height;
            const int index = (y * 13 + x) * 4;
            CHECK(pixels[index] == (inside ? 255 : 0));
            CHECK(pixels[index + 3] == (inside ? 255 : 0));
        }
        CHECK(glGetError() == GL_NO_ERROR);
    }
    glDisable(GL_SCISSOR_TEST);
    ClipWaylandModelRegion({3, 2, 5, 4}, 13, 9);
    CHECK(!glIsEnabled(GL_SCISSOR_TEST));
    // Editor frame is drawn after clipping, including outside the model clip,
    // and restores the renderer's GL state.
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST); glScissor(1, 2, 3, 4);
    glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE);
    glClearColor(.25f, .5f, .75f, 1);
    DrawWaylandEditFrame({{0, 0, 13, 1}, {-5, 3, 7, 2}}, 13, 9);
    GLint frameScissor[4]; GLboolean frameMask[4]; GLfloat frameColor[4];
    glGetIntegerv(GL_SCISSOR_BOX, frameScissor); glGetBooleanv(GL_COLOR_WRITEMASK, frameMask);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, frameColor);
    CHECK(glIsEnabled(GL_SCISSOR_TEST));
    CHECK(frameScissor[0] == 1 && frameScissor[1] == 2 && frameScissor[2] == 3 && frameScissor[3] == 4);
    CHECK(!frameMask[0] && frameMask[1] && !frameMask[2] && !frameMask[3]);
    CHECK(frameColor[0] == .25f && frameColor[1] == .5f && frameColor[2] == .75f);
    std::vector<unsigned char> framePixels(13 * 9 * 4);
    glReadPixels(0, 0, 13, 9, GL_RGBA, GL_UNSIGNED_BYTE, framePixels.data());
    CHECK(framePixels[(8 * 13) * 4 + 3] == 255); // top edge
    CHECK(framePixels[(5 * 13) * 4 + 3] == 255); // clipped left handle
    CHECK(framePixels[(4 * 13 + 5) * 4 + 3] == 0); // untouched background
    CHECK(glGetError() == GL_NO_ERROR);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
}
