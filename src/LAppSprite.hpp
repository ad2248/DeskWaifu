/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#pragma once

#include <GL/glew.h>


#include "LAppSprite_Common.hpp"

/**
* @brief 实现精灵绘制的类。
*
* 管理纹理 ID 和矩形区域。
*
*/
class LAppSprite : public LAppSprite_Common
{
public:
    /**
    * @brief 构造函数。
    *
    * @param[in]       x X 坐标。
    * @param[in]       y Y 坐标。
    * @param[in]       width 宽度。
    * @param[in]       height 高度。
    * @param[in]       textureId 纹理 ID。
    * @param[in]       programId 着色器程序 ID。
    */
    LAppSprite(float x, float y, float width, float height, GLuint textureId, GLuint programId);

    /**
    * @brief 析构函数。
    */
    ~LAppSprite();

    /**
    * @brief 执行绘制。
    *
    */
    // 目标尺寸由调用方提供，与窗口独立。
    void Render(int maxWidth, int maxHeight) const;

    /**
    * @brief 使用指定纹理绘制。
    *
    */
    void RenderImmidiate(GLuint textureId, const GLfloat uvVertex[8], int maxWidth, int maxHeight) const;

    /**
    * @brief 构造函数。
    *
    * @param[in]       pointX X 坐标。
    * @param[in]       pointY Y 坐标。
    */
    bool IsHit(float pointX, float pointY, int maxWidth, int maxHeight) const;

    /**
     * @brief 设置颜色。
     *
     * @param[in]       r 红色分量（0.0~1.0）。
     * @param[in]       g 绿色分量（0.0~1.0）。
     * @param[in]       b 蓝色分量（0.0~1.0）。
     * @param[in]       a 透明度（0.0~1.0）。
     */
    void SetColor(float r, float g, float b, float a);

    /**
     * @brief 重新设置尺寸。
     *
     * @param[in]       x X 坐标。
     * @param[in]       y Y 坐标。
     * @param[in]       width 宽度。
     * @param[in]       height 高度。
     */
    void ResetRect(float x, float y, float width, float height);

private:
    Rect _rect;          ///< 矩形
    int _positionLocation;  ///< 位置属性。
    int _uvLocation;        ///< UV 属性。
    int _textureLocation;   ///< 纹理属性。
    int _colorLocation;     ///< 颜色属性。

    float _spriteColor[4];  ///< 显示颜色。
};
