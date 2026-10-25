#include <GL/glew.h>
#include "ReadRgbaFrame.hpp"

bool InitializeRenderGl()
{
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) return false;
    while (glGetError() != GL_NO_ERROR) {}
    return true;
}

bool ReadRgbaFrame(unsigned framebuffer, int width, int height, std::vector<unsigned char>& pixels)
{
    if (!framebuffer || width <= 0 || height <= 0) return false;
    GLint previous, alignment, rowLength, skipRows, skipPixels, packBuffer;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous);
    glGetIntegerv(GL_PACK_ALIGNMENT, &alignment);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &rowLength);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &skipRows);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &skipPixels);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1); glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_ROWS, 0); glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    pixels.resize(static_cast<size_t>(width) * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, previous);
    glPixelStorei(GL_PACK_ALIGNMENT, alignment); glPixelStorei(GL_PACK_ROW_LENGTH, rowLength);
    glPixelStorei(GL_PACK_SKIP_ROWS, skipRows); glPixelStorei(GL_PACK_SKIP_PIXELS, skipPixels);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, packBuffer);
    return glGetError() == GL_NO_ERROR;
}
