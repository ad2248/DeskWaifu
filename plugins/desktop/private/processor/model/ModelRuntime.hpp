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
    // 在当前 GL 上下文中启动 Cubism 并加载初始模型；任一步失败返回 false。
    bool Initialize(const ModelRuntimeOptions& options, int width, int height);
    // 执行模型操作 JSON；返回含 ok 状态或错误原因的业务回复。
    QJsonObject Execute(const std::string& json);
    // 返回当前模型能力的 CPU 值快照，不暴露 SDK 对象。
    ModelMetadata Metadata() const;
    // 绘制并读回完整帧；目标绑定或读回失败时抛出异常。
    std::shared_ptr<RenderFrame> Render(const ModelRenderState& state, ModelRenderTarget& target);
    // contextCurrent 表示本线程持有正确上下文；无法安全析构 GL 资源时返回 false。
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
