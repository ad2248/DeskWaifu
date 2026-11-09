#pragma once

#include "processor/interaction/window/DesktopWindow.hpp"
#include "ModelWindowState.hpp"
#include "TransformEditor.hpp"
#include "processor/interaction/config/HoverOpacitySettings.hpp"
#include "processor/interaction/config/OutputSettings.hpp"
#include "processor/interaction/window/CompositorAdapter.hpp"
#include "processor/model/RenderFrame.hpp"
#include "contracts/InputSample.hpp"
#include "event/ProcessorEventLoop.hpp"

// InteractionProcessor 的内部状态，不是处理器之间的服务定位器。
// 窗口、编辑事务、配置和外部控制的生命周期同属 GUI 交互域。
class InteractionContext final : public IApplicationRuntime, public IDesktopWindowEvents
{
public:
    InteractionContext(ProcessorEventLoop& loop, IProcessorPlugin& owner,
        std::unique_ptr<DesktopWindow> window, LAppConfig config);
    // 初始化窗口、变换和输出选择；必要步骤失败时返回 false。
    bool Start(int width, int height);
    // 关闭窗口并完成状态机收尾；应先停止并回收模型渲染资源。
    void Stop();
    ApplicationService& Service() { return service; }
    TransformEditor& Settings() { return settings; }
    HoverOpacitySettings& HoverSettings() { return hover; }
    LAppConfig& Config() { return config; }
    int GetWindowWidth() const { return window->Width(); }
    int GetWindowHeight() const { return window->Height(); }
    std::vector<OutputSnapshot> Outputs() const { return window->Outputs(); }
    OutputId CurrentOutput() const { return window->CurrentOutput(); }
    // 切换并持久化显示器名称，返回切换或保存结果，并发布新的窗口信息。
    AppResult SelectOutput(const std::string& name);
    // 请求进入关闭态，但不在此处拆除窗口或回收渲染资源。
    void AppEnd();
    // 根据全局光标跨屏移动本地编辑草稿，重基裁剪区域并暂停自动保存。
    void MoveScreenDrag();
    // 未编辑时尝试切到合成器当前焦点输出；缺少能力时不操作。
    void FocusOutput();
    // 将编辑事务状态同步到窗口策略状态机。
    void SyncMode();
    // 广播当前窗口尺寸及输出名称，供输入域更新坐标映射。
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
