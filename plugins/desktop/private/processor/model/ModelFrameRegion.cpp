#include "ModelFrameRegion.hpp"
#include "ModelInputRegion.hpp"
#include <GL/glew.h>
#include <vector>

void ClipModelRegion(ModelRegion region, int width, int height) {
    if (region.x == 0 && region.y == 0 && region.width == width && region.height == height) return;

    // 仅在 Cubism 绘制完成后清除区域；其遮罩和离屏绘制自行管理剪裁状态，
    // 不能继承最终显示区域的剪裁设置。
    const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
    GLint scissor[4];
    GLfloat clearColor[4];
    GLboolean colorMask[4];
    glGetIntegerv(GL_SCISSOR_BOX, scissor);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
    glEnable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 0);
    for (const auto& outside : OutsideModelRegion(region, width, height)) {
        if (outside.width <= 0 || outside.height <= 0) continue;
        const auto box = ModelRegionToOpenGL(outside, height);
        glScissor(box.x, box.y, box.width, box.height);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
    if (!scissorEnabled) glDisable(GL_SCISSOR_TEST);
    glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
}

void DrawModelEditFrame(const std::vector<ModelRegion>& handles, int width, int height) {
    const GLboolean enabled = glIsEnabled(GL_SCISSOR_TEST);
    GLint scissor[4]; GLfloat color[4]; GLboolean mask[4];
    glGetIntegerv(GL_SCISSOR_BOX, scissor); glGetFloatv(GL_COLOR_CLEAR_VALUE, color);
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    glEnable(GL_SCISSOR_TEST); glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(.1f, .75f, 1.f, 1.f);
    for (auto handle : handles) {
        auto box = IntersectModelRegions(handle, {0, 0, width, height});
        if (box.width <= 0 || box.height <= 0) continue;
        box = ModelRegionToOpenGL(box, height);
        glScissor(box.x, box.y, box.width, box.height); glClear(GL_COLOR_BUFFER_BIT);
    }
    glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
    if (!enabled) glDisable(GL_SCISSOR_TEST);
    glClearColor(color[0], color[1], color[2], color[3]);
    glColorMask(mask[0], mask[1], mask[2], mask[3]);
}

std::vector<ModelRegion> ReadModelInputRegion(unsigned int framebuffer, int width, int height,
    ModelRegion clip, std::vector<unsigned char>& pixels) {
    if (width <= 0 || height <= 0) return {};
    GLint previous;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    pixels.resize(size_t(width) * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, previous);
    return ExtractModelInputRegion(pixels.data(), width, height, clip);
}
