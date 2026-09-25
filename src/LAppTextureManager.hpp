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
* @brief テクスチャ管理クラス
*
* 画像読み込み、管理を行うクラス。
*/
class LAppTextureManager : public LAppTextureManager_Common
{
public:
    /**
    * @brief コンストラクタ
    */
    LAppTextureManager();

    /**
    * @brief デストラクタ
    *
    */
    ~LAppTextureManager();

    using TextureLease = TextureResidency<GLuint>::Lease;
    TextureLease AcquireTexture(const std::string& fileName);
    std::size_t ResidentBytes() const { return _residency.ResidentBytes(); }
    std::size_t ResidentCount() const { return _residency.ResidentCount(); }

private:
    std::pair<GLuint, std::size_t> UploadTexture(const std::string& fileName);
    TextureResidency<GLuint> _residency;
};
