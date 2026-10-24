#include <signal.h>
/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "LAppDelegate.hpp"
#include "MiniMaxSpeechSource.hpp"
#include "AudioPlayer.hpp"
#include "SpeechPlayer.hpp"
#include "OcrClient.hpp"
#include "processor/ProcessorRuntime.hpp"
#include "processor/ipc/IpcProcessor.hpp"
#include <QClipboard>
#include <QGuiApplication>
#include <cmath>
#include <chrono>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <unistd.h>
#include <libgen.h>
#include <GL/glew.h>

#include "LAppView.hpp"
#include "LAppPal.hpp"
#include "ModelFrameRegion.hpp"
#include "ModelInputRegion.hpp"
#include "LAppDefine.hpp"
#include "LAppConfig.hpp"
#include "LAppLive2DManager.hpp"
#include "LAppModel.hpp"
#include "LAppTextureManager.hpp"
#include "LAppIPC.hpp"
#include "AppCommandHandler.hpp"
#include "ModelScroll.hpp"
#include "RuntimeLifecycle.hpp"
#include "GpuTeardown.hpp"
#include "InputController.hpp"
#include "ModelFrame.hpp"

using namespace Csm;
using namespace std;
using namespace LAppDefine;

namespace {
    LAppDelegate* s_instance = NULL;
    volatile sig_atomic_t pendingToggle = 0;
    volatile sig_atomic_t pendingFocusMove = 0;
    uint32_t ClickClock()
    {
        using namespace std::chrono;
        return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    }
    double SettingsClock()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}

static void HandleToggleSignal(int) {
    pendingToggle = 1;
}

static void HandleFocusSignal(int) {
    pendingFocusMove = 1;
}

LAppDelegate* LAppDelegate::GetInstance()
{
    if (s_instance == NULL)
    {
        s_instance = new LAppDelegate();
    }

    return s_instance;
}

void LAppDelegate::ReleaseInstance()
{
    if (s_instance != NULL)
    {
        delete s_instance;
    }

    s_instance = NULL;
}

bool LAppDelegate::Initialize()
{
    if (!_windowState.Send(ModelWindowState::Event::Initialize)) return false;
    const bool initialized = InitializeRuntime();
    _windowState.Send(initialized ? ModelWindowState::Event::Initialized : ModelWindowState::Event::Close);
    return initialized && _windowState.Running();
}

bool LAppDelegate::InitializeRuntime()
{
    if (DebugLogEnable)
    {
        LAppPal::PrintLogLn("START");
    }

    signal(SIGUSR1, HandleToggleSignal);
    signal(SIGUSR2, HandleFocusSignal);


    _windowWidth = RenderTargetWidth;
    _windowHeight = RenderTargetHeight;

    if (!_window->Initialize(RenderTargetWidth, RenderTargetHeight, *this)) {
        return false;
    }

    glewExperimental = GL_TRUE;
    GLenum err = glewInit();
    if (err != GLEW_OK) {
        if (DebugLogEnable)
        {
            LAppPal::PrintLogLn("Can't initilize glew. Error: %s", glewGetErrorString(err));
        }
        return false;
    }

    //テクスチャサンプリング設定
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

    //透過設定
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glViewport(0, 0, _windowWidth, _windowHeight);

    // Cubism3の初期化
    SetExecuteAbsolutePath();
    LAppPal::SetResourceBasePath(_executeAbsolutePath);
    InitializeCubism();
    _cubismStarted = true;
    auto& config = LAppConfig::GetInstance();
    _settings.reset(new TransformEditor(_service, config));
    _settings->SetEditChanged([this] { SyncInteractionMode(); });
    const auto& values = config.values;
    if (!_settings->Initialize({{values.modelScale, values.modelX, values.modelY}, bool(values.modelRegion), values.modelRegion.value_or(ModelRegion{})})) {
        LAppPal::PrintLogLn("[Config] Invalid model transform; zoom must be finite and within 0.1..10");
        return false;
    }
    _hoverSettings.reset(new HoverOpacitySettings(config));
    _speechSource.reset(new MiniMaxSpeechSource(QString::fromStdString(config.values.minimaxApiKey)));
    _audioPlayer.reset(new AudioPlayer);
    _speechPlayer.reset(new SpeechPlayer(*_speechSource, *_audioPlayer));
    QObject::connect(_speechPlayer.get(), &SpeechPlayer::ErrorOccurred, _speechPlayer.get(),
        [](SpeechId, const SpeechError& error) {
            LAppPal::PrintLogLn("[Speech] %s", error.message.toUtf8().constData());
        });
    _processors.reset(new ProcessorRuntime(*_speechPlayer,
        [this] { _speechSource->SetApiKey(QString::fromStdString(LAppConfig::GetInstance().values.minimaxApiKey)); },
        [] { return QGuiApplication::clipboard()->image(); }));
    const auto hoverResult = _hoverSettings->Refresh();
    if (!hoverResult.ok) LAppPal::PrintLogLn("[Config] %s", hoverResult.error.c_str());
    OutputSettings outputSettings(*_window, *_settings, config);
    const auto outputResult = outputSettings.Initialize();
    if (!outputResult.ok) {
        LAppPal::PrintLogLn("[Config] Display selection failed: %s", outputResult.error.c_str());
        return false;
    }
    if (!outputSettings.Warning().empty())
        LAppPal::PrintLogLn("[Config] Display selection warning: %s", outputSettings.Warning().c_str());
    _windowWidth = _window->Width();
    _windowHeight = _window->Height();
    _lastOutput = _window->CurrentOutput();
    if (!_kwinCursor.Start())
        LAppPal::PrintLogLn("[KWin] Global cursor unavailable: %s; using surface events", _kwinCursor.Error().c_str());
    else LAppPal::PrintLogLn("[KWin] Transient global cursor script loaded");
    auto modelDirs = values.additionalModelDirs;
    for (auto& dir : modelDirs) dir = LAppConfig::ExpandTilde(dir);
    _manager = new LAppLive2DManager(*_textureManager, _windowWidth, _windowHeight,
        {LAppDefine::ModelsDir, modelDirs, values.defaultModel, values.emotionTimeout});
    if (!_manager->HasModels()) return false;
    _view = new LAppView(*_manager, *_textureManager);

    _view->Initialize(_windowWidth, _windowHeight);
    _view->InitializeSprite();

    // Initialize IPC socket server
    _ipc = new LAppIPC();
    _commands = new AppCommandHandler(*this, *_manager);
    _ipc->SetCommandService(*_commands);
    if (!_ipc->Initialize()) {
        LAppPal::PrintLogLn("[IPC] Initialization failed; refusing to start without command endpoint");
        delete _ipc;
        _ipc = nullptr;
        delete _commands;
        _commands = nullptr;
        return false;
    }

    _ipcProcessor.reset(new IpcProcessor(_processors->Loop(), *_ipc));
    return true;
}

OcrClient& LAppDelegate::Ocr() { return _processors->Ocr(); }

bool LAppDelegate::Release()
{
    AppEnd();
    _ipcProcessor.reset();
    _processors.reset();
    _speechPlayer.reset();
    _audioPlayer.reset();
    _speechSource.reset();
    _kwinCursor.Stop();
    const bool released = ReleaseGpuResources(HasGpuResources(_view, _manager,
        _textureManager ? _textureManager->ResidentCount() : 0),
        [this] { return _window->MakeCurrent(); },
        [this] { ReleaseRuntimeInOrder(
        [this] {
            ReleaseCommandTransport(
                [this] { delete _ipc; _ipc = nullptr; },
                [this] { delete _commands; _commands = nullptr; });
        },
        [this] { delete _view; _view = nullptr; },
        [this] { delete _manager; _manager = nullptr; },
        [this] { delete _textureManager; _textureManager = nullptr; },
        [this] { if (_cubismStarted) { CubismFramework::Dispose(); _cubismStarted = false; } },
        [this] { _window->Shutdown(); }); });
    if (released) _windowState.Send(ModelWindowState::Event::Released);
    return released;
}
bool LAppDelegate::Run()
{
    const bool ran = _window->Run([this] {
        if (!_windowState.Running()) return false;
        if (pendingToggle) {
            pendingToggle = 0;
            _service.SetVisibility(_service.Snapshot().hidden);
        }
        if (_window->OutputNeedsRebind()) _settings->AbortScreenEdit();
        _window->ReconcileOutputs();
        if (_lastOutput != _window->CurrentOutput()) {
            _settings->AbortScreenEdit();
            _settings->PauseAutosave();
            _lastOutput = _window->CurrentOutput();
        }
        if (!_windowState.Running()) return false;
        if (_input.TapExpired(ClickClock())) FlushPendingTap();
        int width = _window->Width();
        int height = _window->Height();

        if (width != _windowWidth || height != _windowHeight) {
            if (_settings->ScreenEditing()) _settings->AbortScreenEdit();
            _windowState.Send(ModelWindowState::Event::CancelGesture);
            CancelInteraction();
        }
        ResizeView(width, height);

        glViewport(0, 0, _windowWidth, _windowHeight);

        if (pendingFocusMove) {
            pendingFocusMove = 0;
            const auto focused = _compositor.FocusedOutput();
            if (focused && !_settings->Editing()) {
                if (!_outputController.Focus(focused.value))
                    LAppPal::PrintLogLn("[Wayland] Focused output not available: %s", focused.value.c_str());
                else _settings->PauseAutosave();
            } else LAppPal::PrintLogLn("[Wayland] Focus capability unavailable (%d)", static_cast<int>(focused.status));
        }
        TrackGlobalCursor();
        _lastOutput = _window->CurrentOutput();

        if (_input.IsCaptured() && _settings->Dirty()) _settings->ScheduleSave(SettingsClock(), .5);
        _settings->Tick(SettingsClock());

        LAppPal::UpdateTime();

        // 画面の初期化 -> Transparent!
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glClearDepth(1.0);

        const auto state = _service.Snapshot();
        const bool opacityFrame = (_hoverSettings->Enabled() || _hoverSettings->IdleOpacity() != 1.0f)
            && !state.hidden && _view->BeginOpacityFrame();
        RenderModelFrame(_service, _settings->Editing(),
            [this](ModelTransform transform) { UpdateCursorLook(transform); },
            [this](ModelTransform transform, const ModelFrameInput& input) {
                _view->Render(transform, input);
            });

        if (!state.hidden || _settings->ScreenEditing()) {
            const auto& settings = _settings->Display();
            const auto region = ResolveModelRegion(settings.cropped, settings.region,
                _windowWidth, _windowHeight);
            // Sample only the model, before display opacity or editor handles.
            const auto modelRegions = ReadModelInputRegion(opacityFrame ? _view->OpacityFramebuffer() : _window->Framebuffer(),
                _windowWidth, _windowHeight, region, _inputPixels);
            const auto handles = _settings->ScreenEditing() && settings.cropped
                ? CropInteraction::Handles(settings.region) : std::vector<ModelRegion>{};
            if (opacityFrame)
                _view->PresentOpacityFrame(_hoverOpacity.Resolve(_hoverSettings->Enabled(), _hoverSettings->Opacity(),
                    modelRegions, handles, _hoverSettings->IdleOpacity()));
            ClipModelRegion(region, _windowWidth, _windowHeight);
            if (_settings->ScreenEditing())
                DrawModelEditFrame(handles, _windowWidth, _windowHeight);
            _window->SetInputRegion(ModelFrameInputRegions(_settings->ScreenEditing(),
                _windowWidth, _windowHeight, modelRegions));
        }
        if (state.hidden && !_settings->ScreenEditing()) _window->SetInputRegion({});
        return _windowState.Running();
    });
    const bool released = Release();
    if (released) LAppPal::PrintLogLn("[APP]GPU resources released before window teardown");
    else LAppPal::PrintLogLn("[APP]Could not restore GPU context for teardown; resources left to process cleanup");
    LAppDelegate::ReleaseInstance();
    return ran && released;
}
LAppDelegate::LAppDelegate():
    _cubismOption(),
    _window(CreateDesktopWindow()),
    _outputController(*_window),
    _compositor(CompositorAdapter::Detect(), _compositorTransport),
    _view(nullptr),
    _manager(nullptr),
    _ipc(nullptr),
    _commands(nullptr),
    _service(*this),
    
    _mouseX(0.0f),
    _mouseY(0.0f),
    _windowWidth(0),
    _windowHeight(0),
    _lookCenterX(0.5f),
    _lookCenterY(0.5f)
{
    _executeAbsolutePath = "";
    _textureManager = new LAppTextureManager();
    QObject::connect(&_windowState, &ModelWindowState::InteractionCancelled,
        &_windowState, [this] { CancelInteraction(); });
}

LAppDelegate::~LAppDelegate()
{

}

void LAppDelegate::InitializeCubism()
{
    //setup cubism
    _cubismOption.LogFunction = LAppPal::PrintMessage;
    _cubismOption.LoggingLevel = LAppDefine::CubismLoggingLevel;
    _cubismOption.LoadFileFunction = LAppPal::LoadFileAsBytes;
    _cubismOption.ReleaseBytesFunction = LAppPal::ReleaseBytes;
    Csm::CubismFramework::StartUp(&_cubismAllocator, &_cubismOption);

    //Initialize cubism
    CubismFramework::Initialize();

    LAppPal::UpdateTime();
}

void LAppDelegate::TrackGlobalCursor()
{
    const auto outputs = _window->Outputs();
    const auto* current = _outputController.Current(outputs);
    _kwinCursor.Poll(current ? current->name : "");
    double x = 0, y = 0;
    if (_kwinCursor.LocalPosition(_windowWidth, _windowHeight, x, y))
        _cursorLook.LocalPosition(x, y);
}

void LAppDelegate::UpdateCursorLook(ModelTransform transform)
{
    if (!_manager || !_cursorLook.HasPosition()) return;
    const auto outputs = _window->Outputs();
    const auto* current = _outputController.Current(outputs);
    if (!current) return;
    const auto look = _cursorLook.LookAt(current->x, current->y, _windowWidth,
        _windowHeight, _lookCenterX, _lookCenterY, transform, LAppConfig::GetInstance().values.mouseLookRadius);
    _manager->OnDrag(look.first, look.second);
}

void LAppDelegate::ResizeView(int width, int height)
{
    if ((_windowWidth == width && _windowHeight == height) || width <= 0 || height <= 0) return;
    _view->Initialize(width, height);
    _view->ResizeSprite();
    _view->DestroySpriteRenderTarget();
    _manager->SetRenderTargetSize(width, height);
    _windowWidth = width;
    _windowHeight = height;
}

void LAppDelegate::MoveScreenDrag()
{
    const auto cursor = _compositor.GlobalCursor();
    if (!cursor) return; // Surface-local editing remains usable without this capability.
    const auto outputs = _window->Outputs();
    const auto oldId = _window->CurrentOutput();
    const auto* next = OutputController::Find(outputs,
        OutputController::At(outputs, cursor.value.first, cursor.value.second, oldId));
    if (!next || next->id == oldId) return;

    auto draft = _settings->Display();
    if (!_outputController.MoveTo(cursor.value.first, cursor.value.second, draft.transform)) return;

    // Crop coordinates belong to the old screen, not to the moving model.
    // Start with the destination's full extent so the moved model stays visible
    // and the user can continue adjusting the crop before explicitly saving.
    draft.region = {0, 0, _window->Width(), _window->Height()};
    _settings->Update(_settings->Owner(), draft);
    _settings->PauseAutosave();
    // This deliberate rebind must not be treated as a hotplug/resize cancellation
    // on the next frame. The edit remains explicit: moving displays never saves it.
    _lastOutput = _window->CurrentOutput();
    ResizeView(_window->Width(), _window->Height());
    SetMousePosition(cursor.value.first - next->x, cursor.value.second - next->y);
}

void LAppDelegate::CancelInteraction()
{
    _input = InputController{};
    _cropInteraction = ScreenCropInteraction{};
    if (_manager) _manager->OnDrag(0.0f, 0.0f);
    if (_window->KeyboardGrabbed()) _window->SetKeyboardGrab(false);
}

void LAppDelegate::SyncInteractionMode()
{
    if (!_settings || !_windowState.Running()) return;
    _windowState.Send(!_settings->Editing() ? ModelWindowState::Event::NormalMode :
        _settings->ScreenEditing() ? ModelWindowState::Event::ScreenMode : ModelWindowState::Event::PanelMode);
}

void LAppDelegate::StartCropGesture()
{
    const auto handle = _cropInteraction.Handle();
    _windowState.Send(handle == CropInteraction::Model ? ModelWindowState::Event::DragModel :
        handle == CropInteraction::Move ? ModelWindowState::Event::MoveCrop : ModelWindowState::Event::ResizeCrop);
}

void LAppDelegate::OutputSwitchStarted()
{
    if (_ipcProcessor) _ipcProcessor->SetPaused(true);
    _windowState.Send(ModelWindowState::Event::SwitchOutput);
    if (_settings && _window->OutputNeedsRebind()) _settings->AbortScreenEdit();
}

void LAppDelegate::OutputSwitchFinished()
{
    _windowState.Send(ModelWindowState::Event::OutputReady);
    SyncInteractionMode();
    if (_ipcProcessor) _ipcProcessor->SetPaused(false);
}

void LAppDelegate::PointerButton(int button, bool pressed)
{
    if (_view == NULL || !_windowState.Running())
    {
        return;
    }
    
    if (!_windowState.Normal())
    {
        if (button == 0 && _windowState.ScreenEditing()) {
            if (pressed) {
                if (_windowState.GestureActive()) return;
                _cropInteraction.Press(_settings->Display(), int(_mouseX), int(_mouseY));
                StartCropGesture();
            } else {
                if (!_windowState.GestureActive()) return;
                const bool moveOutput = _settings->LocalScreenEditing() && _cropInteraction.DraggingModel();
                const auto result = _cropInteraction.Release(*_settings, ClickClock(), int(_mouseX), int(_mouseY));
                _windowState.Send(ModelWindowState::Event::Release);
                if (!result.ok) LAppPal::PrintLogLn("[Crop] Save failed: %s", result.error.c_str());
                if (moveOutput) MoveScreenDrag();
            }
        }
        return; // No model taps, skin changes or keyboard grabs during editing.
    }
    if (button == 0)
    {
        if (pressed)
        {
            if (!_windowState.Send(ModelWindowState::Event::Press)) return;
            _input.Press(static_cast<int>(_mouseX), static_cast<int>(_mouseY));
            _view->OnTouchesBegan(_mouseX, _mouseY);
        }
        else
        {
            if (_windowState.Current() == ModelWindowState::State::Pressed)
            {
                const auto result = _input.Release(ClickClock(), static_cast<int>(_mouseX),
                    static_cast<int>(_mouseY), [this](int x, int y)
                    {
                        _view->OnTouchesEnded(x, y, _service.Snapshot().transform);
                    });
                _windowState.Send(ModelWindowState::Event::Release);
                if (result == InputController::ReleaseResult::DoubleClick)
                {
                    _manager->OnDrag(0.0f, 0.0f);
                    _window->SetKeyboardGrab(true);
                    if (_processors) _processors->SpeakGreeting(QStringLiteral("姐姐，你好喵"));
                }
                else if (result == InputController::ReleaseResult::Drag)
                {
                    _manager->OnDrag(0.0f, 0.0f); // End look
                }
            }
        }
    }
    else if (button == 1 && !pressed)
    {
        // Switch Models
        LAppPal::PrintLogLn("[Event] Switch Model Triggered");
          LAppPal::PrintLogLn("[Event] Switch Model Triggered");
          const auto state = _service.Snapshot();
          if (state.modelCount) _service.SelectModel((state.modelIndex + 1) % state.modelCount);
    }
    else if (button == 2 && pressed)
    {
        // Switch Skin
        LAppPal::PrintLogLn("[Event] Switch Skin Triggered");
        _service.SwitchSkin();
    }
}

void LAppDelegate::FlushPendingTap()
{
    if (!_windowState.Normal()) return;
    _input.FlushTap([this](int x, int y)
    {
        _view->OnTouchesEnded(x, y, _service.Snapshot().transform);
    });
}

void LAppDelegate::ResetKeyboardState()
{
    _input.ResetKeyboard();
}

void LAppDelegate::OnModelKey(uint32_t code, bool pressed)
{
    if (!_windowState.Normal()) return;
    LAppModel* model = _window->KeyboardGrabbed()
        ? _manager->GetModel(0) : nullptr;
    const auto* bindings = model ? &model->Hotkeys().Bindings() : nullptr;
    const ModelKeyDecision decision = _input.Key(
        _window->KeyboardGrabbed(), code, pressed, bindings);
    if (decision.action == ModelKeyDecision::ReleaseGrab)
        _window->SetKeyboardGrab(false);
    else if (decision.action == ModelKeyDecision::Trigger)
    {
        const auto result = _service.TriggerHotkey(decision.bindingId);
        if (!result.ok) LAppPal::PrintLogLn("[Hotkeys] Trigger failed for binding %s: %s",
            decision.bindingId.c_str(), result.error.c_str());
    }
}

void LAppDelegate::PointerMoved(double x, double y)
{
    SetMousePosition(x, y);

    if (_view == NULL || !_windowState.Running())
    {
        return;
    }

    if (!_windowState.Normal()) {
        if (_windowState.ScreenEditing() && _windowState.GestureActive())
            _cropInteraction.Move(*_settings, int(x), int(y), _windowHeight);
        return;
    }
    if (_windowState.Current() == ModelWindowState::State::Pressed)
    {
        if (_settings) {
            if (_cropInteraction.StartModelDrag(*_settings, _input.StartX(), _input.StartY(),
                    int(x), int(y), _windowWidth, _windowHeight)) {
                StartCropGesture();
            }
            return;
        }
    }
}

void LAppDelegate::SetMousePosition(double x, double y)
{
    _hoverOpacity.Move(x, y);
    _mouseX = static_cast<float>(x);
    _mouseY = static_cast<float>(y);
    _cursorLook.LocalPosition(x, y);
}

void LAppDelegate::PointerScroll(double vertical)
{
    if (!_view || !_windowState.Running()) return;
    const ModelTransform transform = ZoomModelFromScroll(
        _service.Snapshot().transform, _view, _mouseX, _mouseY,
        static_cast<float>(vertical));
    if (!_windowState.Normal()) {
        if (_windowState.ScreenEditing()) { auto next = _settings->Display(); next.transform = transform; _settings->Update(_settings->Owner(), next); }
    } else {
        _service.SetTransform(transform);
        if (_settings) _settings->ScheduleSave(SettingsClock(), .5);
    }
}

bool LAppDelegate::SelectModel(int index) { return _manager && _manager->ChangeScene(index); }
int LAppDelegate::ModelIndex() const { return _manager ? _manager->GetSceneIndex() : -1; }
int LAppDelegate::ModelCount() const { return _manager ? _manager->GetModelDirSize() : 0; }

bool LAppDelegate::SetHidden(bool hidden)
{
    if (hidden) {
        if (!_window->SetInputRegion({})) return false;
        _windowState.Send(ModelWindowState::Event::CancelGesture);
        CancelInteraction();
        _hoverOpacity.Leave();
        _window->SetKeyboardGrab(false);
        _input.ResetKeyboard();
        return true;
    }
    return _window->Available();
}

bool LAppDelegate::PlayMotion(const std::string& group, int index, int priority)
{
    LAppModel* model = _manager ? _manager->GetModel(0) : nullptr;
    if (!model) return false;
    if (group.empty())
        return model->StartRandomMotion(MotionGroupTapBody, priority) != InvalidMotionQueueEntryHandleValue;
    for (const auto& motion : model->GetMotionList())
        if (motion.group == group && motion.index == index)
            return model->StartMotion(group.c_str(), index, priority) != InvalidMotionQueueEntryHandleValue;
    return false;
}

bool LAppDelegate::SetExpression(const std::string& id)
{
    LAppModel* model = _manager ? _manager->GetModel(0) : nullptr;
    if (!model) return false;
    const auto expressions = model->GetExpressionIds();
    if (std::find(expressions.begin(), expressions.end(), id) == expressions.end()) return false;
    model->SetExpression(id.c_str());
    return true;
}

bool LAppDelegate::TriggerHotkey(const std::string& id, std::string& error)
{
    LAppModel* model = _manager ? _manager->GetModel(0) : nullptr;
    if (!model) { error = "no model loaded"; return false; }
    return model->Hotkeys().Trigger(id, error);
}

bool LAppDelegate::SwitchSkin()
{
    if (!_manager || !_manager->GetModel(0)) return false;
    _manager->SwitchSkin();
    return true;
}

void LAppDelegate::GetClientSize(int& rWidth, int& rHeight)
{
    rWidth = GetInstance()->_windowWidth;
    rHeight = GetInstance()->_windowHeight;
}

void LAppDelegate::SetExecuteAbsolutePath()
{
    char path[1024];
    ssize_t len = readlink("/proc/self/exe", path, 1024 - 1);
    if (len < 0) return;
    path[len] = '\0';
    this->_executeAbsolutePath = dirname(path);
    this->_executeAbsolutePath += "/";
}
