#pragma once

#include "RenderFrame.hpp"
#include "LAppAllocator_Common.hpp"
#include <CubismFramework.hpp>

class LAppLive2DManager;
class LAppTextureManager;
class LAppView;
class ModelRenderTarget;

// 仅供工作线程使用。构造、所有操作、渲染和析构均在同一渲染线程执行，
// 不借用 GUI 对象或应用单例。
class ModelRuntime
{
public:
    ModelRuntime();
    bool Initialize(const ModelRuntimeOptions& options, int width, int height);
    QJsonObject Execute(const std::string& json);
    ModelMetadata Metadata() const;
    std::shared_ptr<RenderFrame> Render(const ModelRenderState& state, ModelRenderTarget& target);
    bool Shutdown(bool contextCurrent);
    ~ModelRuntime();
private:
    void Resize(int width, int height);
    LAppAllocator_Common _allocator;
    Csm::CubismFramework::Option _option{};
    std::unique_ptr<LAppTextureManager> _textures;
    std::unique_ptr<LAppLive2DManager> _manager;
    std::unique_ptr<LAppView> _view;
    int _width = 0, _height = 0;
    bool _started = false;
    std::vector<unsigned char> _inputPixels;
};
