#pragma once

#include "ModelCatalog.hpp"
#include <memory>

class LAppModel;
class LAppTextureManager;

class ModelLoader
{
public:
    ModelLoader(LAppTextureManager& textures, int width, int height, float emotionTimeout)
        : _textures(textures), _width(width), _height(height), _emotionTimeout(emotionTimeout) {}
    std::unique_ptr<LAppModel> Load(const ModelDescriptor& descriptor) const;
    void SetViewport(int width, int height) { _width = width; _height = height; }

private:
    LAppTextureManager& _textures;
    int _width;
    int _height;
    float _emotionTimeout;
};
