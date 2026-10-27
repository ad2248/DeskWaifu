#pragma once

#include "processor/interaction/window/DesktopWindow.hpp"
#include "ModelWindowState.hpp"
#include "TransformEditor.hpp"
#include "processor/interaction/config/HoverOpacitySettings.hpp"
#include "processor/interaction/config/OutputSettings.hpp"
#include "processor/interaction/window/CompositorAdapter.hpp"
#include "contracts/RenderFrame.hpp"
#include "contracts/InputSample.hpp"
#include "event/ProcessorEventLoop.hpp"

// InteractionProcessor 的内部状态，不是处理器之间的服务定位器。
// 窗口、编辑事务、配置和外部控制的生命周期同属 GUI 交互域。
class InteractionContext final : public IApplicationRuntime, public IDesktopWindowEvents
{
public:
    InteractionContext(ProcessorEventLoop& loop, IProcessorPlugin& owner,
        std::unique_ptr<DesktopWindow> window, LAppConfig config);
    bool Start(int width, int height);
    void Stop();
    ApplicationService& Service() { return service; }
    TransformEditor& Settings() { return settings; }
    HoverOpacitySettings& HoverSettings() { return hover; }
    LAppConfig& Config() { return config; }
    int GetWindowWidth() const { return window->Width(); }
    int GetWindowHeight() const { return window->Height(); }
    std::vector<OutputSnapshot> Outputs() const { return window->Outputs(); }
    OutputId CurrentOutput() const { return window->CurrentOutput(); }
    AppResult SelectOutput(const std::string& name);
    void AppEnd();
    void MoveScreenDrag();
    void FocusOutput();
    void SyncMode();
    void PublishWindow();
    int ModelIndex() const override { return metadata.index; }
    int ModelCount() const override { return metadata.names.size(); }
    bool SetHidden(bool hidden) override;

    std::unique_ptr<DesktopWindow> window;
    LAppConfig config;
    ModelMetadata metadata;
    ModelWindowState mode;
    ApplicationService service;
    TransformEditor settings;
    HoverOpacitySettings hover;
    OutputController outputs;
    BoundedCompositorTransport transport;
    CompositorAdapter compositor;
    OutputId lastOutput = 0;
    int frameWidth = 0, frameHeight = 0;

private:
    void Input(const QString& kind, QVariantMap data = {});
    void PointerEntered(double x, double y) override { Input("enter", {{"x", x}, {"y", y}}); }
    void PointerLeft() override { Input("leave"); }
    void PointerMoved(double x, double y) override { Input("move", {{"x", x}, {"y", y}}); }
    void PointerButton(int button, bool pressed) override { Input("button", {{"button", button}, {"pressed", pressed}}); }
    void PointerScroll(double value) override { Input("scroll", {{"vertical", value}}); }
    void KeyboardChanged() override { Input("keyboard_reset"); }
    void KeyboardKey(uint32_t code, bool pressed) override { Input("key", {{"code", code}, {"pressed", pressed}}); }
    void SurfaceClosed() override { AppEnd(); }
    void OutputSwitchStarted() override;
    void OutputSwitchFinished() override;
    ProcessorEventLoop& _loop;
    IProcessorPlugin& _owner;
    InputSamplePtr _move;
};
