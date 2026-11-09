/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#pragma once

#include <GL/glew.h>

#include <Math/CubismMatrix44.hpp>
#include <Math/CubismViewMatrix.hpp>
#include "CubismFramework.hpp"
#include <Rendering/OpenGL/CubismRenderTarget_OpenGLES2.hpp>

#include "LAppView_Common.hpp"
#include "contracts/ModelTransform.hpp"
#include "contracts/ModelFrameInput.hpp"
#include "processor/model/ModelOpacityFrame.hpp"

class TouchManager_Common;
class LAppSprite;
class LAppSpriteShader;
class LAppModel;
class LAppLive2DManager;
class LAppTextureManager;

/**
* @brief 绘制类。
*/
class LAppView : public LAppView_Common
{
public:

    /**
     * @brief LAppModel 的渲染目标。
     */
    enum SelectTarget
    {
        SelectTarget_None,                ///< 渲染到默认帧缓冲。
        SelectTarget_ModelFrameBuffer,    ///< 渲染到各 LAppModel 持有的帧缓冲。
        SelectTarget_ViewFrameBuffer,     ///< 渲染到 LAppView 持有的帧缓冲。
    };

    /**
    * @brief 构造函数。
    */
    LAppView(LAppLive2DManager& manager, LAppTextureManager& textures);

    /**
    * @brief 析构函数。
    */
    ~LAppView();

    /**
    * @brief 初始化。
    */
    virtual void Initialize(int width, int height) override;

    /**
    * @brief 执行绘制。
    */
    void Render(ModelTransform transform, const ModelFrameInput& input);

    bool BeginOpacityFrame() { return _opacityFrame.Begin(_width, _height); }
    GLuint OpacityFramebuffer() const { return _opacityFrame.Framebuffer(); }
    void PresentOpacityFrame(float opacity);

    /**
    * @brief 初始化图像。
    */
    void InitializeSprite();

    /**
     * @brief 重新设置精灵相关尺寸。
     */
    void ResizeSprite();

    /**
    * @brief 触摸开始时调用。
    *
    * @param[in]       pointX 屏幕 X 坐标。
    * @param[in]       pointY 屏幕 Y 坐标。
    */
    void OnTouchesBegan(float pointX, float pointY) const;

    /**
    * @brief 触摸期间指针移动时调用。
    *
    * @param[in]       pointX 屏幕 X 坐标。
    * @param[in]       pointY 屏幕 Y 坐标。
    */
    void OnTouchesMoved(float pointX, float pointY) const;

    /**
    * @brief 触摸结束时调用。
    *
    * @param[in]       pointX 屏幕 X 坐标。
    * @param[in]       pointY 屏幕 Y 坐标。
    */
    void OnTouchesEnded(float pointX, float pointY, ModelTransform transform) const;

    /**
     * @brief 每个模型绘制前调用。
     */
    void PreModelDraw(LAppModel &refModel);

    /**
     * @brief 每个模型绘制后调用。
     */
    void PostModelDraw(LAppModel &refModel);

    /**
     * @brief 在将模型绘制到其他渲染目标的示例中，
     *        确定绘制时的 alpha。
     */
    float GetSpriteAlpha(int assign) const;

    /**
     * @brief 切换渲染目标。
     */
    void SwitchRenderingTarget(SelectTarget targetType);

    /**
     * @brief 设置非默认渲染目标的背景清除颜色。
     * @param[in]   r 红色（0.0~1.0）。
     * @param[in]   g 绿色（0.0~1.0）。
     * @param[in]   b 蓝色（0.0~1.0）。
     */
    void SetRenderTargetClearColor(float r, float g, float b);

    /**
     * @brief 销毁精灵的渲染目标。
     */
    void DestroySpriteRenderTarget();

private:
    LAppLive2DManager& _manager;
    LAppTextureManager& _textures;
    int _width = 0;
    int _height = 0;
    TouchManager_Common* _touchManager;                 ///< 触摸管理器。
    LAppSprite* _back;                       ///< 背景图片。
    LAppSprite* _gear;                       ///< 齿轮图片。
    LAppSprite* _power;                      ///< 电源图片。

    // 渲染到其他目标时使用。
    LAppSprite* _renderSprite;                                  ///< 按模式绘制 _renderBuffer 的纹理。
    Csm::Rendering::CubismRenderTarget_OpenGLES2 _renderBuffer;   ///< 按模式将 Cubism 模型结果渲染到此处。
    SelectTarget _renderTarget;     ///< 渲染目标选项。
    float _clearColor[4];           ///< 渲染目标的清除颜色。

    LAppSpriteShader* _spriteShader;  ///< 负责创建着色器的委托对象。
    ModelOpacityFrame _opacityFrame;
};
