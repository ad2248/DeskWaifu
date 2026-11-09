#pragma once

#include "processor/Processor.hpp"
#include "processor/model/RenderFrame.hpp"
#include "processor/model/ModelExchange.hpp"
#include "InteractionContext.hpp"
#include "CursorLook.hpp"
#include "processor/interaction/window/DesktopWindow.hpp"
#include "ModelWindowState.hpp"
#include "ScreenCropInteraction.hpp"
#include <QTimer>
#include <functional>
#include <optional>

class AppCommandHandler;

// GUI 状态所有者；窗口操作和编辑事务不进入渲染线程。
// 生产构造器独占 Context；策略测试构造器可借用同域辅助对象。
class InteractionProcessor final : public Processor
{
public:
    InteractionProcessor(ProcessorEventLoop& loop, DesktopWindow& window,
        ModelWindowState& mode, ApplicationService& service, TransformEditor& settings,
        IProcessorPlugin& model);
    InteractionProcessor(ProcessorEventLoop& loop, IProcessorPlugin& model,
        std::unique_ptr<DesktopWindow> window, LAppConfig config, std::shared_ptr<ModelExchange> exchange);
    // 生产实例初始化交互上下文及首帧共享状态；测试借用实例返回 false。
    bool Start(int width, int height);
    // 阻塞呈现直至退出；窗口运行或模型渲染失败时返回 false。
    bool Run();
    // 非阻塞启动呈现，返回是否启动成功；结束时回调合并后的窗口与渲染结果。
    bool StartFrames(std::function<void(bool)> finished);
    // 拆除窗口资源；必须在模型渲染线程完成 GPU 清理后调用。
    void ShutdownWindow();
    // 创建交给模型处理器独占的离屏渲染目标，不转移窗口所有权。
    std::unique_ptr<ModelRenderTarget> RenderTarget(const std::string& device);
    ~InteractionProcessor() override;
    bool HandleEvent(const ProcessorEvent& event) override;
    void PointerButton(int button, bool pressed, std::optional<uint32_t> time = {});
    void PointerMoved(double x, double y);
    void PointerScroll(double vertical);
    void Position(double x, double y);
    void Enter(double x, double y) { _inside = true; Position(x, y); }
    void Leave() { _inside = false; }
    void CursorPosition(double x, double y) { _cursor.LocalPosition(x, y); }
    void Key(uint32_t code, bool pressed);
    void ResetKeyboard() { _input.ResetKeyboard(); }
    void Cancel();
    void Stop() noexcept override;
    bool Captured() const { return _input.IsCaptured(); }
    // 每次用当前布局重新计算，静止光标也能响应变换、缩放和配置变化。
    void ApplyLook(ModelRenderState& state, const OutputSnapshot* output,
        float centerX, float centerY, float radius) const;
    // 查询窗口局部位置的模型命中；不可用时返回空结果并按需请求新命中接口。
    ModelHitResult HitAt(double x, double y);
    // 查询当前指针命中；指针不在表面内时清除 inside 与命中部位。
    ModelHitResult Hover();
    // 将编辑裁剪、操作柄及悬停或全屏透明度写入待发布的帧状态。
    void PrepareFrame(ModelRenderState& state, bool hoverEnabled, float hoverOpacity, float idleOpacity);
    // 返回实际接收输入的区域；隐藏时为空，屏幕编辑时覆盖整个窗口。
    std::vector<ModelRegion> InputRegions(const ModelRenderState& state, const std::vector<ModelRegion>& regions) const;

private:
    friend struct ProcessorTestAccess;
    void StartCropGesture();
    void FlushTap();
    void Tap(int x, int y);
    void Operation(QJsonObject operation);
    void RequestHitTest();
    void ConnectSources();
    std::unique_ptr<EventEndpoint> _notifications;
    bool Present();
    std::unique_ptr<InteractionContext> _context;
    std::unique_ptr<AppCommandHandler> _commands;
    std::shared_ptr<ModelExchange> _exchange;
    DesktopWindow& _window;
    ModelWindowState& _mode;
    ApplicationService& _service;
    TransformEditor& _settings;
    IProcessorPlugin& _model;
    ModelMetadata _metadata;
    InputController _input;
    ScreenCropInteraction _crop;
    CursorLook _cursor;
    QTimer _tapTimer;
    float _x = 0, _y = 0;
    bool _stopped = false;
    bool _inside = false, _hitRequested = false;
    bool _fullscreen = false;
    ModelHitTest _hitTest;
    bool _renderingOk = true;
};
