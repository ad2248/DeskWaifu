#include <GL/glew.h>
#include <EGL/egl.h>
#include "LAppSprite.hpp"
#include <cmath>
#include <iostream>
#include <vector>
#include <utility>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "Failed at line " << __LINE__ << ": " << #condition << '\n'; return 1; } } while (false)

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
    const EGLint size[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    const EGLSurface surface = eglCreatePbufferSurface(display, config, size);
    const EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    CHECK(surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT);
    CHECK(eglMakeCurrent(display, surface, surface, context));
    glewExperimental = GL_TRUE;
    CHECK(glewInit() == GLEW_OK);
    while (glGetError() != GL_NO_ERROR) {}

    const GLuint vertex = Compile(GL_VERTEX_SHADER,
        "#version 120\nattribute vec2 position; attribute vec2 uv; varying vec2 texCoord;"
        "void main(){texCoord=uv;gl_Position=vec4(position,0.0,1.0);}");
    const GLuint fragment = Compile(GL_FRAGMENT_SHADER,
        "#version 120\nuniform sampler2D texture; uniform vec4 baseColor; varying vec2 texCoord;"
        "void main(){gl_FragColor=texture2D(texture,texCoord)*baseColor;}");
    CHECK(vertex && fragment);
    const GLuint program = glCreateProgram();
    glAttachShader(program, vertex); glAttachShader(program, fragment); glLinkProgram(program);
    GLint linked;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    CHECK(linked);
    glDeleteShader(vertex); glDeleteShader(fragment);
    glUseProgram(program);

    GLuint textures[3], framebuffer;
    glGenTextures(3, textures);
    for (const auto texture : textures) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    const unsigned char straight[] = {255, 128, 64, 255};
    const unsigned char premultiplied[] = {128, 64, 32, 128};
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, straight);
    glBindTexture(GL_TEXTURE_2D, textures[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, premultiplied);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    CHECK(framebuffer != 0);
    glEnable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    {
        LAppSprite sprite(3, 4, 4, 4, textures[0], program);
        const GLfloat uv[] = {1, 1, 0, 1, 0, 0, 1, 0};
        for (const auto dimensions : {std::pair<int, int>{16, 12}, {8, 20}}) {
            const int width = dimensions.first, height = dimensions.second;
            glBindTexture(GL_TEXTURE_2D, textures[2]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[2], 0);
            CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
            glViewport(0, 0, width, height);
            // A non-centered rect distinguishes top-left input from GL's Y axis.
            sprite.ResetRect(3, 4, 4, 4);
            CHECK(sprite.IsHit(3, height - 4, width, height));
            CHECK(!sprite.IsHit(3, height - 9, width, height));
            const int centerX = width / 2, centerY = height / 2;
            sprite.ResetRect(centerX, centerY, 4, 4);
            CHECK(sprite.IsHit(centerX, height - centerY, width, height));
            CHECK(!sprite.IsHit(0, height - centerY, width, height));
            CHECK(!sprite.IsHit(centerX, 0, width, height));
            for (const bool immediate : {false, true}) {
                glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
                if (immediate) {
                    // LAppView's opacity path scales both premultiplied RGB and alpha.
                    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                    sprite.SetColor(.5f, .5f, .5f, .5f);
                    sprite.RenderImmidiate(textures[1], uv, width, height);
                } else {
                    sprite.SetColor(1, 1, 1, .5f);
                    sprite.Render(width, height);
                }
                std::vector<unsigned char> pixels(width * height * 4);
                glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
                for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                    const bool inside = x >= centerX - 2 && x < centerX + 2 && y >= centerY - 2 && y < centerY + 2;
                    const int expected[] = {immediate ? 64 : 128, immediate ? 32 : 64, immediate ? 16 : 32, 64};
                    for (int channel = 0; channel < 4; ++channel)
                        CHECK(std::abs(int(pixels[(y * width + x) * 4 + channel]) - (inside ? expected[channel] : 0)) <= 1);
                }
                CHECK(glGetError() == GL_NO_ERROR);
            }
            // Invalid targets neither divide by zero nor draw/hit the old rect.
            glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
            sprite.Render(0, height);
            sprite.Render(width, -1);
            sprite.RenderImmidiate(textures[1], uv, width, 0);
            sprite.RenderImmidiate(textures[1], uv, -1, height);
            CHECK(!sprite.IsHit(centerX, height - centerY, 0, height));
            CHECK(!sprite.IsHit(centerX, height - centerY, width, -1));
            unsigned char pixel[4];
            glReadPixels(centerX, centerY, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
            CHECK(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 0);
            CHECK(glGetError() == GL_NO_ERROR);
        }
    }
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(3, textures);
    glDeleteProgram(program);
    CHECK(glGetError() == GL_NO_ERROR);
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
}
