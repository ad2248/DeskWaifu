#pragma once

#include <GL/glew.h>
#include <Rendering/OpenGL/CubismRenderTarget_OpenGLES2.hpp>

// 模型的外层帧，与 Cubism 内部遮罩和渲染目标独立。
// 在应用仅影响显示的透明度之前，从此帧采样输入区域。
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
            // 上次呈现可能仍绑定着旧纹理；调整尺寸会删除它，
            // 因此不能将该旧标识重新绑定为新纹理。
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
