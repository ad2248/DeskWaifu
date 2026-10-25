#pragma once

#include "processor/Processor.hpp"
#include "processor/model/RenderFrame.hpp"
#include "CursorLook.hpp"
#include "DesktopWindow.hpp"
#include "ModelWindowState.hpp"
#include "ScreenCropInteraction.hpp"
#include <QTimer>
#include <functional>

// GUI 线程中的交互策略；窗口操作和编辑事务不进入渲染线程。
// 注入的窗口、状态、服务和模型必须存活到 Stop 及模型工作线程收尾之后。
class InteractionProcessor final : public Processor
{
public:
    InteractionProcessor(ProcessorEventLoop& loop, DesktopWindow& window,
        ModelWindowState& mode, ApplicationService& service, TransformEditor& settings,
        IProcessor& model, std::function<ModelMetadata()> metadata,
        std::function<void()> moveScreen, std::function<void()> greet);
    ~InteractionProcessor() override;
    bool HandleEvent(const ProcessorEvent& event) override;
    void PointerButton(int button, bool pressed);
    void PointerMoved(double x, double y);
    void PointerScroll(double vertical);
    void Position(double x, double y);
    void CursorPosition(double x, double y) { _cursor.LocalPosition(x, y); }
    void Key(uint32_t code, bool pressed);
    void ResetKeyboard() { _input.ResetKeyboard(); }
    void Cancel();
    void Stop();
    bool Captured() const { return _input.IsCaptured(); }
    // 每次用当前布局重新计算，静止光标也能响应变换、缩放和配置变化。
    void ApplyLook(ModelRenderState& state, const OutputSnapshot* output,
        float centerX, float centerY, float radius) const;

private:
    void StartCropGesture();
    void FlushTap();
    void Tap(int x, int y);
    void Operation(QJsonObject operation);
    DesktopWindow& _window;
    ModelWindowState& _mode;
    ApplicationService& _service;
    TransformEditor& _settings;
    IProcessor& _model;
    std::function<ModelMetadata()> _metadata;
    std::function<void()> _moveScreen, _greet;
    InputController _input;
    ScreenCropInteraction _crop;
    CursorLook _cursor;
    QTimer _tapTimer;
    float _x = 0, _y = 0;
    bool _stopped = false;
};
