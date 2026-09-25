/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "LAppTextureManager.hpp"
#include <iostream>
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include "LAppPal.hpp"

LAppTextureManager::LAppTextureManager()
    : LAppTextureManager_Common()
    , _residency([](GLuint id) { glDeleteTextures(1, &id); })
{
}

LAppTextureManager::~LAppTextureManager() = default;

LAppTextureManager::TextureLease LAppTextureManager::AcquireTexture(const std::string& fileName)
{
    return _residency.Acquire(fileName, [this](const std::string& path)
    {
        return UploadTexture(path);
    });
}

std::pair<GLuint, std::size_t> LAppTextureManager::UploadTexture(const std::string& fileName)
{
    GLuint textureId;
    int width, height, channels;
    unsigned int size;
    unsigned char* png;
    unsigned char* address;

    address = LAppPal::LoadFileAsBytes(fileName, &size);
    if (!address) return {};

    // png情報を取得する
    png = stbi_load_from_memory(
        address,
        static_cast<int>(size),
        &width,
        &height,
        &channels,
        STBI_rgb_alpha);
    if (!png) { LAppPal::ReleaseBytes(address); return {}; }
    {

#ifdef PREMULTIPLIED_ALPHA_ENABLE
        unsigned int* fourBytes = reinterpret_cast<unsigned int*>(png);
        for (int i = 0; i < width * height; i++)
        {
            unsigned char* p = png + i * 4;
            fourBytes[i] = LAppTextureManager_Common::Premultiply(p[0], p[1], p[2], p[3]);
        }
#endif
    }

    // OpenGL用のテクスチャを生成する
    glGenTextures(1, &textureId);
    if (!textureId) {
        stbi_image_free(png);
        LAppPal::ReleaseBytes(address);
        return {};
    }
    glBindTexture(GL_TEXTURE_2D, textureId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, png);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    // 解放処理
    stbi_image_free(png);
    LAppPal::ReleaseBytes(address);

    return std::make_pair(textureId,
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
}
