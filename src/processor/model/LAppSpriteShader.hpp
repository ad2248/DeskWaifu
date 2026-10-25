/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#pragma once

#include <GL/glew.h>

#include <Utils/CubismString.hpp>
#include "CubismFramework.hpp"

/**
* @brief 保存精灵着色器设置的类。
*/
class LAppSpriteShader
{
public:
    /**
     * @brief 构造函数。
     */
    LAppSpriteShader();

    /**
     * @brief 析构函数。
     */
    ~LAppSpriteShader();

    /**
     * @brief 获取着色器 ID。
     */
    GLuint GetShaderId() const;

private:
    /**
    * @brief 创建着色器。
    */
    GLuint CreateShader();

    /**
     * @brief CreateShader 内部的错误检查函数。
     */
    bool CheckShader(GLuint shaderId);

    /**
     * @brief 编译着色器。
     *        编译失败时返回 0。
     *
     * @param[in]       filename 着色器文件名。
     * @param[in]       shaderType 待创建的着色器类型。
     */
    GLuint CompileShader(Csm::csmString filename, GLenum shaderType);

    GLuint _programId;                       ///< 着色器 ID。
};
