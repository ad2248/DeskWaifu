#include "InteractionContext.hpp"
#include <chrono>
#include <iostream>

InteractionContext::InteractionContext(ProcessorEventLoop& loop, IProcessorPlugin& owner,
    std::unique_ptr<DesktopWindow> backend, LAppConfig initial)
    : window(std::move(backend)), config(std::move(initial)), service(*this),
      settings(service, config), hover(config), outputs(*window),
      compositor(CompositorAdapter::Detect(), transport), _loop(loop), _owner(owner)
{
    settings.SetEditChanged([this] { SyncMode(); });
}

bool InteractionContext::Start(int width, int height)
{
    if (!mode.Send(ModelWindowState::Event::Initialize)) return false;
    if (!window->Initialize(width, height, *this)) return false;
    const auto& v = config.values;
    if (!settings.Initialize({{v.modelScale, v.modelX, v.modelY}, bool(v.modelRegion), v.modelRegion.value_or(ModelRegion{})})) {
        std::cerr << "[Config] Invalid model transform; zoom must be finite and within 0.1..10\n";
        return false;
    }
    const auto hoverResult = hover.Refresh();
    if (!hoverResult.ok) std::cerr << "[Config] " << hoverResult.error << '\n';
    OutputSettings selection(*window, settings, config);
    const auto outputResult = selection.Initialize();
    if (!outputResult.ok) { std::cerr << "[Config] Display selection failed: " << outputResult.error << '\n'; return false; }
    if (!selection.Warning().empty()) std::cerr << "[Config] Display selection warning: " << selection.Warning() << '\n';
    lastOutput = window->CurrentOutput();
    frameWidth = window->Width(); frameHeight = window->Height();
    mode.Send(ModelWindowState::Event::Initialized);
    PublishWindow();
    return true;
}

void InteractionContext::Stop()
{
    AppEnd();
    window->Shutdown();
    mode.Send(ModelWindowState::Event::Released);
}

void InteractionContext::AppEnd() { mode.Send(ModelWindowState::Event::Close); }
void InteractionContext::SyncMode()
{
    if (!mode.Running()) return;
    mode.Send(!settings.Editing() ? ModelWindowState::Event::NormalMode :
        settings.ScreenEditing() ? ModelWindowState::Event::ScreenMode : ModelWindowState::Event::PanelMode);
}

bool InteractionContext::SetHidden(bool hidden)
{
    if (!hidden) return window->Available();
    if (!window->SetInputRegion({})) return false;
    mode.Send(ModelWindowState::Event::CancelGesture);
    window->SetKeyboardGrab(false);
    Input("leave"); Input("keyboard_reset");
    return true;
}

void InteractionContext::Input(const QString& kind, QVariantMap data)
{
    if (kind == "move") {
        const double x = data.value("x").toDouble(), y = data.value("y").toDouble();
        if (!_move || _move->consumed) {
            _move = std::make_shared<InputSample>(InputSample{x, y, false, false});
            _loop.Post({"input.native_move", &_owner, nullptr, {{"sample", QVariant::fromValue(_move)}}});
        } else { _move->x = x; _move->y = y; }
        return;
    }
    _move.reset(); // 离散事件为连续移动的合并屏障。
    data.insert("kind", kind);
    data.insert("time", static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()));
    _loop.Post({"input.native", &_owner, nullptr, std::move(data)});
}

void InteractionContext::PublishWindow()
{
    const auto all = window->Outputs();
    const auto* current = OutputController::Find(all, window->CurrentOutput());
    _loop.Post({"window.state", &_owner, nullptr, {{"width", window->Width()}, {"height", window->Height()},
        {"output", QString::fromStdString(current ? current->name : "")}}});
}

void InteractionContext::OutputSwitchStarted()
{
    _move.reset();
    _loop.Post({"window.switch", &_owner, nullptr, {{"paused", true}}});
    mode.Send(ModelWindowState::Event::SwitchOutput);
    if (window->OutputNeedsRebind()) settings.AbortScreenEdit();
}
void InteractionContext::OutputSwitchFinished()
{
    mode.Send(ModelWindowState::Event::OutputReady); SyncMode();
    PublishWindow();
    _loop.Post({"window.switch", &_owner, nullptr, {{"paused", false}}});
}

AppResult InteractionContext::SelectOutput(const std::string& name)
{
    OutputSettings selection(*window, settings, config);
    const auto result = selection.Select(name);
    PublishWindow();
    return result;
}

void InteractionContext::FocusOutput()
{
    const auto focused = compositor.FocusedOutput();
    if (focused && !settings.Editing() && outputs.Focus(focused.value)) settings.PauseAutosave();
}

void InteractionContext::MoveScreenDrag()
{
    const auto cursor = compositor.GlobalCursor();
    if (!cursor) return;
    const auto all = window->Outputs();
    const auto old = window->CurrentOutput();
    const auto* next = OutputController::Find(all, OutputController::At(all, cursor.value.first, cursor.value.second, old));
    if (!next || next->id == old) return;
    auto draft = settings.Display();
    if (!outputs.MoveTo(cursor.value.first, cursor.value.second, draft.transform)) return;
    draft.region = {0, 0, window->Width(), window->Height()};
    settings.Update(settings.Owner(), draft); settings.PauseAutosave();
    lastOutput = window->CurrentOutput();
    // 主动跨屏移动已重基草稿，下一帧不能当成热插拔尺寸变化而取消事务。
    frameWidth = window->Width(); frameHeight = window->Height();
    Input("position", {{"x", cursor.value.first - next->x}, {"y", cursor.value.second - next->y}});
    PublishWindow();
}
