#pragma once

#include "processor/Processor.hpp"
#include "contracts/RenderFrame.hpp"
#include "contracts/ModelExchange.hpp"
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
        IProcessor& model);
    InteractionProcessor(ProcessorEventLoop& loop, IProcessor& model,
        std::unique_ptr<DesktopWindow> window, LAppConfig config, std::shared_ptr<ModelExchange> exchange);
    bool Start(int width, int height);
    bool Run();
    void ShutdownWindow();
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
    void Stop();
    bool Captured() const { return _input.IsCaptured(); }
    // 每次用当前布局重新计算，静止光标也能响应变换、缩放和配置变化。
    void ApplyLook(ModelRenderState& state, const OutputSnapshot* output,
        float centerX, float centerY, float radius) const;
    ModelHitResult HitAt(double x, double y);
    ModelHitResult Hover();
    void PrepareFrame(ModelRenderState& state, bool hoverEnabled, float hoverOpacity, float idleOpacity);
    std::vector<ModelRegion> InputRegions(const ModelRenderState& state, const std::vector<ModelRegion>& regions) const;

private:
    friend struct ProcessorTestAccess;
    void StartCropGesture();
    void FlushTap();
    void Tap(int x, int y);
    void Operation(QJsonObject operation);
    void RequestHitTest();
    void ConnectSources();
    bool Present();
    void SyncSpeechSettings();
    std::unique_ptr<InteractionContext> _context;
    std::unique_ptr<AppCommandHandler> _commands;
    std::shared_ptr<ModelExchange> _exchange;
    DesktopWindow& _window;
    ModelWindowState& _mode;
    ApplicationService& _service;
    TransformEditor& _settings;
    IProcessor& _model;
    ModelMetadata _metadata;
    InputController _input;
    ScreenCropInteraction _crop;
    CursorLook _cursor;
    QTimer _tapTimer;
    float _x = 0, _y = 0;
    bool _stopped = false;
    bool _inside = false, _hitRequested = false;
    ModelHitTest _hitTest;
    bool _renderingOk = true;
    std::optional<QString> _speechKey;
};
