#pragma once

#include <GL/glew.h>
#include <Rendering/OpenGL/CubismRenderTarget_OpenGLES2.hpp>

// An outer model frame, independent of Cubism's internal masks/render targets.
// Input is sampled from this frame before applying any display-only opacity.
class ModelOpacityFrame
{
public:
    ~ModelOpacityFrame() { _buffer.DestroyRenderTarget(); }
    ModelOpacityFrame() = default;
    ModelOpacityFrame(const ModelOpacityFrame&) = delete;
    ModelOpacityFrame& operator=(const ModelOpacityFrame&) = delete;

    bool Begin(int width, int height)
    {
        if (width <= 0 || height <= 0) return false;
        const bool resize = !_buffer.IsValid() || _buffer.GetBufferWidth() != unsigned(width)
            || _buffer.GetBufferHeight() != unsigned(height);
        if (resize) {
            GLint texture;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
            // A previous presentation may have left our old texture bound;
            // resizing deletes it, so it must not be rebound as a new texture.
            if (GLuint(texture) == _buffer.GetColorBuffer()) texture = 0;
            const bool created = _buffer.CreateRenderTarget(width, height);
            glBindTexture(GL_TEXTURE_2D, texture);
            if (!created) return false;
        }
        _buffer.BeginDraw();
        if (resize && glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            _buffer.EndDraw();
            _buffer.DestroyRenderTarget();
            return false;
        }
        glViewport(0, 0, width, height);
        _buffer.Clear(0, 0, 0, 0);
        return true;
    }

    template<typename Composite>
    void Present(float opacity, Composite composite)
    {
        _buffer.EndDraw();
        composite(_buffer.GetColorBuffer(), opacity);
    }

    GLuint Framebuffer() const { return _buffer.GetRenderTexture(); }

private:
    Csm::Rendering::CubismRenderTarget_OpenGLES2 _buffer;
};
