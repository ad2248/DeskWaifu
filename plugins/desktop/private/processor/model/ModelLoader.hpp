#pragma once

#include "ModelCatalog.hpp"
#include <memory>

class LAppModel;
class LAppTextureManager;

// 在渲染线程加载模型，借用纹理管理器；管理器必须比加载器及所加载模型存活更久。
class ModelLoader {
public:
    ModelLoader(LAppTextureManager& textures, int width, int height, float emotionTimeout)
        : _textures(textures)
        , _width(width)
        , _height(height)
        , _emotionTimeout(emotionTimeout)
    {
    }
    // 读取配置并创建模型，成功转移所有权；文件无效或资源加载失败返回空指针。
    std::unique_ptr<LAppModel> Load(const ModelDescriptor& descriptor) const;
    void SetViewport(int width, int height)
    {
        _width = width;
        _height = height;
    }

private:
    LAppTextureManager& _textures;
    int _width;
    int _height;
    float _emotionTimeout;
};
