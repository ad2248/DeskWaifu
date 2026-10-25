#include "InteractionProcessor.hpp"
#include "contracts/InputSample.hpp"
#include "AppCommandHandler.hpp"
#include "processor/Events.hpp"
#include <GL/glew.h>
#include <QJsonDocument>
#include <chrono>
#include <iostream>
#include <algorithm>

namespace {
uint32_t ClickClock()
{
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
double SettingsClock()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

InteractionProcessor::InteractionProcessor(ProcessorEventLoop& loop, DesktopWindow& window,
    ModelWindowState& mode, ApplicationService& service, TransformEditor& settings,
    IProcessor& model)
    : Processor(loop), _window(window), _mode(mode), _service(service), _settings(settings),
      _model(model)
{ ConnectSources(); }

InteractionProcessor::InteractionProcessor(ProcessorEventLoop& loop, IProcessor& model,
    std::unique_ptr<DesktopWindow> window, LAppConfig config, std::shared_ptr<ModelExchange> exchange)
    : Processor(loop), _context(new InteractionContext(loop, *this, std::move(window), std::move(config))),
      _commands(new AppCommandHandler(*_context)), _exchange(std::move(exchange)),
      _window(*_context->window), _mode(_context->mode), _service(_context->service),
      _settings(_context->settings), _model(model)
{ ConnectSources(); }

void InteractionProcessor::ConnectSources()
{
    _tapTimer.setSingleShot(true);
    _tapTimer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&_tapTimer, &QTimer::timeout, this,
        [this] { Send("interaction.tap_timeout", this); });
    QObject::connect(&_mode, &ModelWindowState::InteractionCancelled, this, [this] { Cancel(); });
    RequestHitTest();
}

InteractionProcessor::~InteractionProcessor()
{
    _tapTimer.stop();
}

void InteractionProcessor::Stop()
{
    Cancel();
    _stopped = true;
    if (_context) _context->AppEnd();
}

bool InteractionProcessor::Start(int width, int height)
{
    if (!_context || !_context->Start(width, height)) return false;
    ModelRenderState state;
    state.app = _service.Snapshot(); state.controls = _service.FrameInput();
    state.width = _window.Width(); state.height = _window.Height();
    PrepareFrame(state, _context->hover.Enabled(), _context->hover.Opacity(), _context->hover.IdleOpacity());
    { std::lock_guard<std::mutex> lock(_exchange->mutex); _exchange->state = state; }
    SyncSpeechSettings();
    return true;
}

std::unique_ptr<ModelRenderTarget> InteractionProcessor::RenderTarget(const std::string& device)
{ return _window.RenderTarget(device); }

void InteractionProcessor::SyncSpeechSettings()
{
    const auto key = QString::fromStdString(_context->config.values.minimaxApiKey);
    if (_speechKey && key == *_speechKey) return;
    _speechKey = key;
    Send("speech.configure", nullptr, {{"api_key", key}});
}

void InteractionProcessor::ShutdownWindow() { if (_context) _context->Stop(); }

bool InteractionProcessor::Run()
{
    return _context && _window.Run([this] { return Present(); }) && _renderingOk;
}

bool InteractionProcessor::Present()
{
    if (_stopped || !_mode.Running()) return false;
    if (_window.OutputNeedsRebind()) _settings.AbortScreenEdit();
    _window.ReconcileOutputs();
    if (_context->lastOutput != _window.CurrentOutput()) {
        _settings.AbortScreenEdit(); _settings.PauseAutosave();
        _context->lastOutput = _window.CurrentOutput(); _context->PublishWindow();
    }
    ModelRenderState state;
    { std::lock_guard<std::mutex> lock(_exchange->mutex); state = _exchange->state; }
    const int width = _window.Width(), height = _window.Height();
    if (_context->frameWidth != width || _context->frameHeight != height) {
        if (_settings.ScreenEditing()) _settings.AbortScreenEdit();
        _mode.Send(ModelWindowState::Event::CancelGesture); Cancel();
        _context->PublishWindow();
    }
    _context->frameWidth = width; _context->frameHeight = height;
    state.width = width; state.height = height;
    if (Captured() && _settings.Dirty()) _settings.ScheduleSave(SettingsClock(), .5);
    _settings.Tick(SettingsClock());
    SyncSpeechSettings();
    state.app = _service.Snapshot(); state.controls = _service.FrameInput();
    state.modelToken = _metadata.hotkeys.value("model_token").toString().toStdString();
    PrepareFrame(state, _context->hover.Enabled(), _context->hover.Opacity(), _context->hover.IdleOpacity());
    const auto all = _window.Outputs();
    ApplyLook(state, _context->outputs.Current(all), .5f, .5f, _context->config.values.mouseLookRadius);
    std::shared_ptr<const RenderFrame> frame;
    QString error;
    {
        std::lock_guard<std::mutex> lock(_exchange->mutex);
        _exchange->state = state; frame = _exchange->frame; error = _exchange->error;
    }
    if (!error.isEmpty()) { std::cerr << "[Render] " << error.toStdString() << '\n'; _renderingOk = false; Stop(); return false; }
    const bool usable = frame && frame->width == width && frame->height == height &&
        frame->hidden == state.app.hidden && frame->modelIndex == state.app.modelIndex;
    glViewport(0, 0, width, height);
    glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (usable && (!state.app.hidden || _settings.ScreenEditing()) && !_window.UploadFrame(*frame)) {
        _renderingOk = false; Stop(); return false;
    }
    _window.SetInputRegion(InputRegions(state, usable ? frame->regions : std::vector<ModelRegion>{}));
    return _mode.Running();
}

bool InteractionProcessor::HandleEvent(const ProcessorEvent& event)
{
    if (event.type == "application.quit") { Stop(); return true; }
    if (event.type == "interaction.model") {
        const auto command = QJsonDocument::fromJson(event.data.value("json").toString().toUtf8()).object().value("command").toString();
        if (_stopped || !_mode.Running())
            Send("model.result", event.sender, {{"reply", QJsonObject{{"ok", false}, {"error", "window not running"}}}});
        else if (_settings.Editing() && command != "get_hotkeys")
            Send("model.result", event.sender, {{"reply", QJsonObject{{"ok", false}, {"error", "Finish transform editing before changing model actions"}}}});
        else _loop.Post({"model.operation", event.sender, &_model, event.data});
        return true;
    }
    if (event.type == "model.state" && event.sender == &_model) {
        const auto next = event.data.value("metadata").value<ModelMetadata>();
        if (next.index != _metadata.index || next.hotkeys.value("model_token") != _metadata.hotkeys.value("model_token")) {
            _service.ClearControls(); Cancel();
        }
        _metadata = next;
        if (_context) _context->metadata = next;
        return false; // 状态广播允许其他缓存读取。
    }
    if (event.type == "interaction.command" && _commands) {
        if (_stopped || !_mode.Running()) {
            Send("interaction.result", event.sender, {{"json", QStringLiteral("{\"ok\":false,\"error\":\"interaction stopped\"}")}});
            return true;
        }
        const auto reply = _commands->Execute(event.data.value("json").toString().toStdString(),
            event.data.value("connection").toString().toStdString());
        SyncSpeechSettings();
        Send("interaction.result", event.sender, {{"json", QString::fromStdString(reply)}});
        return true;
    }
    if (event.type == "command.disconnected" && _commands) {
        _commands->Disconnected(event.data.value("connection").toString().toStdString());
        return true;
    }
    if (event.type == "signal.toggle") { _service.SetVisibility(_service.Snapshot().hidden); return true; }
    if (event.type == "signal.focus" && _context) { _context->FocusOutput(); return true; }
    if (event.type == "input.event") {
        if (_stopped) return true;
        const auto kind = event.data.value("kind").toString();
        const double x = event.data.value("x").toDouble(), y = event.data.value("y").toDouble();
        if (kind == "enter") Enter(x, y);
        else if (kind == "leave") Leave();
        else if (kind == "position") Position(x, y);
        else if (kind == "button") PointerButton(event.data.value("button").toInt(), event.data.value("pressed").toBool(),
            event.data.contains("time") ? std::optional<uint32_t>{event.data.value("time").toUInt()} : std::nullopt);
        else if (kind == "scroll") PointerScroll(event.data.value("vertical").toDouble());
        else if (kind == "key") Key(event.data.value("code").toUInt(), event.data.value("pressed").toBool());
        else if (kind == "keyboard_reset") ResetKeyboard();
        return true;
    }
    if (event.type == "input.move") {
        const auto sample = event.data.value("sample").value<InputSamplePtr>();
        if (sample) {
            sample->consumed = true;
            if (!_stopped) {
                if (sample->cursor) CursorPosition(sample->x, sample->y);
                else PointerMoved(sample->x, sample->y);
            }
        }
        return true;
    }
    if (event.type == "model.hit_test.ready" && event.sender == &_model) {
        _hitRequested = false;
        if (!_stopped) _hitTest = event.data.value("callback").value<ModelHitTest>();
        return true;
    }
    if (event.type == "interaction.tap_timeout" && event.sender == this) {
        // 已排队的旧超时不能提前派发新一轮单击。
        if (_input.TapExpired(ClickClock())) FlushTap();
        return true;
    }
    if (event.type == "model.result" && event.sender == &_model) {
        const auto reply = event.data.value("reply").toJsonObject();
        if (!reply.value("ok").toBool())
            std::cout << "[Model] " << reply.value("error").toString().toStdString() << '\n';
        return true;
    }
    return false;
}

void InteractionProcessor::Operation(QJsonObject operation)
{
    if (_stopped) return;
    Send("model.operation", &_model,
        {{"json", QString::fromUtf8(QJsonDocument(operation).toJson(QJsonDocument::Compact))}});
}

void InteractionProcessor::Cancel()
{
    _tapTimer.stop();
    _input = InputController{};
    _crop = ScreenCropInteraction{};
    Operation({{"command", "reset_drag"}});
    if (_window.KeyboardGrabbed()) _window.SetKeyboardGrab(false);
}

void InteractionProcessor::StartCropGesture()
{
    const auto handle = _crop.Handle();
    _mode.Send(handle == CropInteraction::Model ? ModelWindowState::Event::DragModel :
        handle == CropInteraction::Move ? ModelWindowState::Event::MoveCrop : ModelWindowState::Event::ResizeCrop);
}

void InteractionProcessor::PointerButton(int button, bool pressed, std::optional<uint32_t> time)
{
    if (_stopped || !_mode.Running()) return;
    if (_service.Snapshot().hidden && !_settings.ScreenEditing()) return;
    const uint32_t now = time.value_or(ClickClock());
    if (!_mode.Normal()) {
        if (button == 0 && _mode.ScreenEditing()) {
            if (pressed) {
                if (_mode.GestureActive()) return;
                _crop.Press(_settings.Display(), int(_x), int(_y));
                StartCropGesture();
            } else {
                if (!_mode.GestureActive()) return;
                const bool moveOutput = _settings.LocalScreenEditing() && _crop.DraggingModel();
                const auto result = _crop.Release(_settings, now, int(_x), int(_y));
                _mode.Send(ModelWindowState::Event::Release);
                if (!result.ok) std::cout << "[Crop] Save failed: " << result.error << '\n';
                if (moveOutput) {
                    if (_context) _context->MoveScreenDrag();
                    else Send("interaction.move_screen");
                }
            }
        }
        return;
    }
    if (button == 0) {
        if (pressed) {
            if (!_mode.Send(ModelWindowState::Event::Press)) return;
            _input.Press(int(_x), int(_y));
        } else if (_mode.Current() == ModelWindowState::State::Pressed) {
            const auto result = _input.Release(now, int(_x), int(_y),
                [this](int x, int y) { Tap(x, y); });
            _mode.Send(ModelWindowState::Event::Release);
            if (result == InputController::ReleaseResult::DoubleClick) {
                _tapTimer.stop();
                Operation({{"command", "reset_drag"}});
                _window.SetKeyboardGrab(true);
                Send(ProcessorEvents::Speak, nullptr, {{"text", QStringLiteral("姐姐，你好喵")}, {"replace", true}});
            } else if (result == InputController::ReleaseResult::Drag) {
                _tapTimer.stop();
                Operation({{"command", "reset_drag"}});
            } else if (result == InputController::ReleaseResult::TapPending) {
                const uint32_t elapsed = ClickClock() - now;
                _tapTimer.start(elapsed > ModelDoubleClick::Delay ? 0 : ModelDoubleClick::Delay + 1 - elapsed);
            }
        }
    } else if (button == 1 && !pressed) {
        Operation({{"command", "next_model"}});
    } else if (button == 2 && pressed) {
        Operation({{"command", "switch_skin"}});
    }
}

void InteractionProcessor::FlushTap()
{
    if (!_stopped && _mode.Normal()) _input.FlushTap([this](int x, int y) { Tap(x, y); });
}

void InteractionProcessor::Tap(int x, int y)
{
    const auto hit = HitAt(x, y);
    if (!hit.available) return;
    const auto token = _metadata.hotkeys.value("model_token");
    Operation({{"command", "reset_drag"}});
    // Head 优先于 Body，与原有点击策略一致；其他自定义部位仍可供交互查询。
    if (std::find(hit.parts.begin(), hit.parts.end(), "Head") != hit.parts.end())
        Operation({{"command", "random_expression"}, {"model_token", token}});
    else if (std::find(hit.parts.begin(), hit.parts.end(), "Body") != hit.parts.end()) {
        Operation({{"command", "random_motion"}, {"group", "TapBody"}, {"model_token", token}});
        Operation({{"command", "random_expression"}, {"model_token", token}});
    }
}

void InteractionProcessor::RequestHitTest()
{
    if (!_stopped && !_hitRequested)
        _hitRequested = Send("model.hit_test.request", &_model);
}

ModelHitResult InteractionProcessor::HitAt(double x, double y)
{
    if (_stopped || _service.Snapshot().hidden) return {};
    if (!_hitTest) { RequestHitTest(); return {}; }
    auto result = _hitTest({x, y, _window.Width(), _window.Height(),
        _metadata.hotkeys.value("model_token").toString().toStdString()});
    if (!result.available) { _hitTest = {}; RequestHitTest(); }
    return result;
}

ModelHitResult InteractionProcessor::Hover()
{
    auto hit = HitAt(_x, _y);
    if (!_inside) { hit.inside = false; hit.parts.clear(); }
    return hit;
}

void InteractionProcessor::PrepareFrame(ModelRenderState& state, bool hoverEnabled, float hoverOpacity, float idleOpacity)
{
    state.editing = _settings.Editing();
    const auto& settings = _settings.Display();
    state.clip = ResolveModelRegion(settings.cropped, settings.region, state.width, state.height);
    state.handles.clear();
    if (_settings.ScreenEditing() && settings.cropped) state.handles = CropInteraction::Handles(settings.region);
    const auto hit = Hover();
    state.opacity = hoverEnabled && hit.available && hit.inside && !ModelInputRegionContains(state.handles, _x, _y)
        ? hoverOpacity : idleOpacity;
}

std::vector<ModelRegion> InteractionProcessor::InputRegions(const ModelRenderState& state, const std::vector<ModelRegion>& regions) const
{
    return state.app.hidden && !_settings.ScreenEditing() ? std::vector<ModelRegion>{}
        : ModelFrameInputRegions(_settings.ScreenEditing(), state.width, state.height, regions);
}

void InteractionProcessor::Key(uint32_t code, bool pressed)
{
    if (_stopped || !_mode.Normal()) return;
    if (_service.Snapshot().hidden) return;
    const auto& metadata = _metadata;
    const auto decision = _input.Key(_window.KeyboardGrabbed(), code, pressed,
        _window.KeyboardGrabbed() ? &metadata.bindings : nullptr);
    if (decision.action == ModelKeyDecision::ReleaseGrab) _window.SetKeyboardGrab(false);
    else if (decision.action == ModelKeyDecision::Trigger)
        Operation({{"command", "trigger_hotkey"}, {"id", QString::fromStdString(decision.bindingId)},
            {"model_token", metadata.hotkeys.value("model_token")}});
}

void InteractionProcessor::Position(double x, double y)
{
    _x = static_cast<float>(x); _y = static_cast<float>(y);
    _cursor.LocalPosition(x, y);
}

void InteractionProcessor::PointerMoved(double x, double y)
{
    Position(x, y);
    if (_stopped || !_mode.Running()) return;
    if (!_mode.Normal()) {
        if (_mode.ScreenEditing() && _mode.GestureActive())
            _crop.Move(_settings, int(x), int(y), _window.Height());
        return;
    }
    if (_mode.Current() == ModelWindowState::State::Pressed &&
        _crop.StartModelDrag(_settings, _input.StartX(), _input.StartY(),
            int(x), int(y), _window.Width(), _window.Height())) StartCropGesture();
}

void InteractionProcessor::PointerScroll(double vertical)
{
    if (_stopped || !_mode.Running() || _window.Height() <= 0) return;
    if (_service.Snapshot().hidden && !_settings.ScreenEditing()) return;
    const float anchorX = (_x - _window.Width() * .5f) * 2 / _window.Height();
    const float anchorY = -(_y - _window.Height() * .5f) * 2 / _window.Height();
    const auto current = _service.Snapshot().transform;
    const auto transform = ZoomModelAroundPoint(current, anchorX, anchorY, current.scale * (1 + float(vertical) * .1f));
    if (!_mode.Normal()) {
        if (_mode.ScreenEditing()) {
            auto next = _settings.Display(); next.transform = transform;
            _settings.Update(_settings.Owner(), next);
        }
    } else {
        _service.SetTransform(transform);
        _settings.ScheduleSave(SettingsClock(), .5);
    }
}

void InteractionProcessor::ApplyLook(ModelRenderState& state, const OutputSnapshot* output,
    float centerX, float centerY, float radius) const
{
    state.hasLook = !_stopped && output && _cursor.HasPosition() && !state.editing;
    if (!state.hasLook) return;
    const auto look = _cursor.LookAt(output->x, output->y, state.width, state.height,
        centerX, centerY, state.app.transform, radius);
    state.lookX = look.first; state.lookY = look.second;
}
