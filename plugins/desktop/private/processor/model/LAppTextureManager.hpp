/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#pragma once

#include <string>
#include <GL/glew.h>
#include <Type/csmVector.hpp>

#include "LAppTextureManager_Common.hpp"
#include "TextureResidency.hpp"

/**
* @brief 纹理管理类。
*
* 负责图像读取与管理。
*/
// 在渲染线程按文件名共享 GL 纹理；所有租约必须先于管理器释放。
// 获取和最后一次租约释放均需要对应的当前 GL 上下文。
class LAppTextureManager : public LAppTextureManager_Common
{
public:
    /**
    * @brief 构造函数。
    */
    LAppTextureManager();

    /**
    * @brief 析构函数。
    *
    */
    ~LAppTextureManager();

    using TextureLease = TextureResidency<GLuint>::Lease;
    // 返回共享纹理租约；读取、PNG 解码或纹理创建失败时返回空租约。
    TextureLease AcquireTexture(const std::string& fileName);
    std::size_t ResidentBytes() const { return _residency.ResidentBytes(); }
    std::size_t ResidentCount() const { return _residency.ResidentCount(); }

private:
    std::pair<GLuint, std::size_t> UploadTexture(const std::string& fileName);
    TextureResidency<GLuint> _residency;
};
