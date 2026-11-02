#include <GL/glew.h>
#include <EGL/egl.h>
#include "render/OffscreenRenderer.hpp"
#include "contracts/RenderDevice.hpp"
#include <cstdlib>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "Failed at line " << __LINE__ << ": " << #condition << '\n'; return 1; } } while (0)

int main(int argc, char**)
{
    CHECK(ValidRenderDevice("") && ValidRenderDevice("auto") && ValidRenderDevice("0000:00:02.0"));
    for (const auto invalid : {"intel", "0000:00:20.0", "0000:00:02.8", "0000:00:02.00", "0000:GG:02.0"})
        CHECK(!ValidRenderDevice(invalid));
    OffscreenRenderer rejected;
    CHECK(!rejected.Initialize("intel"));
    CHECK(!rejected.MakeCurrent());
    CHECK(rejected.Shutdown() && rejected.Shutdown());
    if (argc > 1) return 0;
    const char* device = getenv("DESKWAIFU_TEST_RENDER_DEVICE");
    if (!device) return 77;
    CHECK(!rejected.Initialize("ffff:ff:1f.7"));
    CHECK(rejected.Shutdown());
    for (int cycle = 0; cycle < 2; ++cycle) {
        OffscreenRenderer renderer;
        CHECK(renderer.Initialize(device));
        const auto current = eglGetCurrentContext();
        bool found = false;
        for (const auto& info : ListRenderDevices()) {
            CHECK(ValidRenderDevice(info.pciAddress) && !info.name.empty());
            if (info.pciAddress == device) found = true;
        }
        CHECK(found && eglGetCurrentContext() == current);
        CHECK(!renderer.BindFrame(0, 10));
        for (int width : {13, 29, 13}) {
            CHECK(renderer.BindFrame(width, 9));
            CHECK(renderer.Width() == width && renderer.Height() == 9);
            CHECK(renderer.Framebuffer() != 0);
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
            glEnable(GL_SCISSOR_TEST);
            glScissor(2, 1, 3, 2);
            glClearColor(.125f, .25f, .5f, .5f);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
            // 调用方的像素打包状态既不能破坏复制，也不能丢失。
            glPixelStorei(GL_PACK_ALIGNMENT, 8);
            glPixelStorei(GL_PACK_ROW_LENGTH, 40);
            glPixelStorei(GL_PACK_SKIP_ROWS, 2);
            glPixelStorei(GL_PACK_SKIP_PIXELS, 3);
            GLuint packBuffer;
            glGenBuffers(1, &packBuffer);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, packBuffer);
            glBufferData(GL_PIXEL_PACK_BUFFER, 16, nullptr, GL_STREAM_READ);
            std::vector<unsigned char> pixels;
            CHECK(renderer.ReadFrame(pixels));
            CHECK(pixels.size() == static_cast<size_t>(width * 9 * 4));
            CHECK(pixels[3] == 0 && pixels[((8 * width) + 2) * 4 + 3] == 0);
            const auto offset = (width + 2) * 4;
            CHECK(pixels[offset] == 32 && pixels[offset + 1] == 64);
            CHECK(pixels[offset + 2] == 128 && pixels[offset + 3] == 128);
            GLint value;
            glGetIntegerv(GL_PACK_ALIGNMENT, &value); CHECK(value == 8);
            glGetIntegerv(GL_PACK_ROW_LENGTH, &value); CHECK(value == 40);
            glGetIntegerv(GL_PACK_SKIP_ROWS, &value); CHECK(value == 2);
            glGetIntegerv(GL_PACK_SKIP_PIXELS, &value); CHECK(value == 3);
            glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &value); CHECK(value == static_cast<GLint>(packBuffer));
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            glDeleteBuffers(1, &packBuffer);
            renderer.DoneCurrent();
            CHECK(eglGetCurrentContext() == EGL_NO_CONTEXT);
            CHECK(renderer.MakeCurrent());
        }
        CHECK(renderer.Shutdown() && renderer.Shutdown());
    }
}
