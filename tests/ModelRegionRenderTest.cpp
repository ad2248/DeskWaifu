#include <GL/glew.h>
#include <EGL/egl.h>
#include "LAppWaylandRegion.hpp"
#include "ModelOpacityFrame.hpp"
#include "HoverOpacity.hpp"
#include <cmath>
#include <iostream>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "Failed at line " << __LINE__ << ": " << #condition << '\n'; return 1; } } while (0)

static GLuint Compile(GLenum type, const char* source)
{
    const GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled) return shader;
    glDeleteShader(shader);
    return 0;
}

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
    glewExperimental = GL_TRUE;
    CHECK(glewInit() == GLEW_OK);
    while (glGetError() != GL_NO_ERROR) {} // Some drivers probe unavailable extensions during glewInit.

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

    // Exercise the same outer frame used by LAppView with real RGBA8 pixels.
    const GLuint vertex = Compile(GL_VERTEX_SHADER,
        "#version 120\nattribute vec2 position; varying vec2 uv;"
        "void main(){uv=(position+1.0)*0.5;gl_Position=vec4(position,0.0,1.0);}");
    const GLuint fragment = Compile(GL_FRAGMENT_SHADER,
        "#version 120\nuniform sampler2D image; uniform float opacity; varying vec2 uv;"
        "void main(){gl_FragColor=texture2D(image,uv)*opacity;}");
    CHECK(vertex && fragment);
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex); glAttachShader(program, fragment); glLinkProgram(program);
    GLint linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    CHECK(linked);
    glDeleteShader(vertex); glDeleteShader(fragment);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glViewport(0, 0, 13, 9);
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "image"), 0);
    const GLfloat quad[] = {-1, -1, 1, -1, 1, 1, -1, 1};
    const GLint position = glGetAttribLocation(program, "position");
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glEnableVertexAttribArray(position);
    glVertexAttribPointer(position, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    std::vector<unsigned char> scratch;
    {
        ModelOpacityFrame frame;
        HoverOpacity hover;
        hover.Enter(4, 4);
        for (const float opacity : {1.0f, 0.5f, 0.01f, 0.0f})
        {
            glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
            CHECK(frame.Begin(13, 9));
            // Model animation/material alpha is 0.8, not forced to 1.
            glClearColor(0.4f, 0.2f, 0, 0.8f); glClear(GL_COLOR_BUFFER_BIT);
            const auto modelRegions = ReadModelInputRegion(frame.Framebuffer(), 13, 9, {2, 1, 8, 7}, scratch);
            CHECK(ModelInputRegionContains(modelRegions, 4, 4));
            CHECK(!ModelInputRegionContains(modelRegions, 1, 4));
            CHECK(hover.Resolve(true, opacity, modelRegions) == opacity);
            bool restored = false;
            frame.Present(opacity, [&](GLuint texture, float alpha) {
                GLint framebuffer, viewport[4];
                glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
                glGetIntegerv(GL_VIEWPORT, viewport);
                restored = framebuffer == 0 && viewport[2] == 13 && viewport[3] == 9;
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, texture);
                glUniform1f(glGetUniformLocation(program, "opacity"), alpha);
                glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
            });
            CHECK(restored);
            unsigned char pixel[4];
            glReadPixels(4, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
            CHECK(std::abs(int(pixel[0]) - int(std::lround(102 * opacity))) <= 1);
            CHECK(std::abs(int(pixel[1]) - int(std::lround(51 * opacity))) <= 1);
            CHECK(std::abs(int(pixel[3]) - int(std::lround(204 * opacity))) <= 1);
            // Explicit source selection works even after presentation; it also
            // restores the window's read binding and does not read faded pixels.
            const auto after = ReadModelInputRegion(frame.Framebuffer(), 13, 9, {2, 1, 8, 7}, scratch);
            CHECK(after.size() == modelRegions.size());
            for (size_t i = 0; i < after.size(); ++i) {
                CHECK(after[i].x == modelRegions[i].x && after[i].y == modelRegions[i].y);
                CHECK(after[i].width == modelRegions[i].width && after[i].height == modelRegions[i].height);
            }
            GLint read;
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
            CHECK(read == 0);
            ClipWaylandModelRegion({2, 1, 8, 7}, 13, 9);
            DrawWaylandEditFrame({{10, 0, 3, 9}}, 13, 9);
            CHECK(!ModelInputRegionContains(modelRegions, 11, 4));
            hover.Leave();
            CHECK(hover.Resolve(true, opacity, modelRegions) == 1);
            hover.Enter(4, 4);
            CHECK(glGetError() == GL_NO_ERROR);
        }
        // Resize and model replacement must not retain pixels from the old frame.
        CHECK(frame.Begin(7, 5));
        GLint textureBinding;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &textureBinding);
        CHECK(textureBinding == 0); // Do not resurrect the deleted, previously bound texture.
        CHECK(ReadModelInputRegion(frame.Framebuffer(), 7, 5, {0, 0, 7, 5}, scratch).empty());
        glClearColor(1, 1, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
        CHECK(ModelInputRegionContains(ReadModelInputRegion(frame.Framebuffer(), 7, 5,
            {0, 0, 7, 5}, scratch), 6, 4));
        frame.Present(1, [](GLuint, float) {});
        CHECK(frame.Begin(7, 5));
        CHECK(ReadModelInputRegion(frame.Framebuffer(), 7, 5, {0, 0, 7, 5}, scratch).empty());
        frame.Present(1, [](GLuint, float) {});
        CHECK(!frame.Begin(0, 5));
    } // Destroy GPU resources before EGL teardown.
    glDeleteProgram(program);
    CHECK(glGetError() == GL_NO_ERROR);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
}
