#include <signal.h>
/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "LAppDelegate.hpp"
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
#include "LAppWaylandRegion.hpp"
#include "LAppDefine.hpp"
#include "LAppConfig.hpp"
#include "LAppLive2DManager.hpp"
#include "LAppModel.hpp"
#include "LAppTextureManager.hpp"
#include "LAppIPC.hpp"
#include "AppCommandHandler.hpp"
#include "ModelScroll.hpp"
#include "ModelHotkeyTrigger.hpp"
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
    if (DebugLogEnable)
    {
        LAppPal::PrintLogLn("START");
    }

    signal(SIGUSR1, HandleToggleSignal);
    signal(SIGUSR2, HandleFocusSignal);


    _windowWidth = RenderTargetWidth;
    _windowHeight = RenderTargetHeight;

    _wlContext.events = this;
    if (!SetupWaylandContext(&_wlContext, RenderTargetWidth, RenderTargetHeight)) {
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
    const auto& config = LAppConfig::GetInstance();
    _settings.reset(new TransformEditor(_service, config.sourcePath.empty() ? LAppConfig::GetDefaultConfigPath() : config.sourcePath));
    if (!_settings->Initialize({{config.modelScale, config.modelX, config.modelY}, config.hasModelRegion, config.modelRegion})) {
        LAppPal::PrintLogLn("[Config] Invalid model transform; zoom must be finite and within 0.1..10");
        return false;
    }
    _lastOutput = _waylandBackend.CurrentOutput();
    _manager = new LAppLive2DManager(*_textureManager, _windowWidth, _windowHeight,
        {LAppDefine::ModelsDir, config.additionalModelDirs, config.defaultModel, config.emotionTimeout});
    if (!_manager->HasModels()) { _isEnd = true; return false; }
    _view = new LAppView(*_manager, *_textureManager);

    _view->Initialize(_windowWidth, _windowHeight);
    _view->InitializeSprite();

    // Initialize IPC socket server
    _ipc = new LAppIPC();
    _commands = new AppCommandHandler(*this, *_manager);
    _ipc->SetCommandService(*_commands);
    _ipc->Initialize();

    return true;
}
bool LAppDelegate::Release()
{
    return ReleaseGpuResources(HasGpuResources(_view, _manager,
        _textureManager ? _textureManager->ResidentCount() : 0),
        [this] { return MakeWaylandGpuContextCurrent(&_wlContext); },
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
        [this] { CleanWaylandContext(&_wlContext); }); });
}
bool LAppDelegate::Run()
{
    while (!_isEnd) 
    {
        if (pendingToggle) {
            pendingToggle = 0;
            _service.SetVisibility(_service.Snapshot().hidden);
        }
        if (wl_display_dispatch_pending(_wlContext.display) == -1) {
            break;
        }
        if (_wlContext.outputNeedsRebind) _settings->AbortScreenEdit();
        _waylandBackend.ReconcileOutputs();
        if (_lastOutput != _waylandBackend.CurrentOutput()) {
            _settings->AbortScreenEdit();
            _settings->PauseAutosave();
            _lastOutput = _waylandBackend.CurrentOutput();
        }
        if (_isEnd) break;
        if (_input.TapExpired(ClickClock())) FlushPendingTap();
        wl_display_flush(_wlContext.display);
        
        int width = _wlContext.width;
        int height = _wlContext.height;

        if (_settings->ScreenEditing() && (width != _windowWidth || height != _windowHeight))
            _settings->AbortScreenEdit();
        if((_windowWidth!=width || _windowHeight!=height) && width>0 && height>0) {
            _view->Initialize(width, height);
            _view->ResizeSprite();
            _view->DestroySpriteRenderTarget();
            _manager->SetRenderTargetSize(width, height);
            _windowWidth = width;
            _windowHeight = height;
        }

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
        _lastOutput = _waylandBackend.CurrentOutput();

        // Poll IPC commands
        _ipc->Poll();
        if (_input.IsCaptured() && _settings->Dirty()) _settings->ScheduleSave(SettingsClock(), .5);
        _settings->Tick(SettingsClock());
        if (_settings->Editing()) {
            _input = InputController{};
            if (_wlContext.keyboardSurface.IsGrabbed()) SetModelKeyboardGrab(&_wlContext, false);
        }
        if (!_settings->ScreenEditing()) _cropInteraction = ScreenCropInteraction{};

        LAppPal::UpdateTime();

        // 画面の初期化 -> Transparent!
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glClearDepth(1.0);

        const auto state = _service.Snapshot();
        const auto& config = LAppConfig::GetInstance();
        const bool opacityFrame = config.hoverOpacityEnabled && !state.hidden && _view->BeginOpacityFrame();
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
            const auto modelRegions = ReadModelInputRegion(opacityFrame ? _view->OpacityFramebuffer() : 0,
                _windowWidth, _windowHeight, region, _inputPixels);
            const auto handles = _settings->ScreenEditing() && settings.cropped
                ? CropInteraction::Handles(settings.region) : std::vector<ModelRegion>{};
            if (opacityFrame)
                _view->PresentOpacityFrame(_hoverOpacity.Resolve(true, config.hoverOpacity, modelRegions, handles));
            ClipWaylandModelRegion(region, _windowWidth, _windowHeight);
            if (_settings->ScreenEditing()) {
                DrawWaylandEditFrame(handles, _windowWidth, _windowHeight);
                UpdateWaylandInputRegion(&_wlContext, modelRegions, handles);
            } else UpdateWaylandInputRegion(&_wlContext, modelRegions);
        }
        if (state.hidden && !_settings->ScreenEditing()) ClearWaylandInputRegion(&_wlContext);
        eglSwapBuffers(_wlContext.egl_display, _wlContext.egl_surface);
        
        if (state.hidden) {
            usleep(33000); // Reduce CPU usage when hidden
        }
    }
    const bool released = Release();
    if (released) LAppPal::PrintLogLn("[APP]GPU resources released before EGL teardown");
    else LAppPal::PrintLogLn("[APP]Could not restore GPU context for teardown; resources left to process cleanup");
    LAppDelegate::ReleaseInstance();
    return released;
}
LAppDelegate::LAppDelegate():
    _cubismOption(),
    _waylandBackend(_wlContext),
    _outputController(_waylandBackend),
    _compositor(CompositorAdapter::Detect(), _compositorTransport),
    _view(nullptr),
    _manager(nullptr),
    _ipc(nullptr),
    _commands(nullptr),
    _service(*this),
    
    _mouseX(0.0f),
    _mouseY(0.0f),
    _isEnd(false),
    _windowWidth(0),
    _windowHeight(0),
    _lookCenterX(0.5f),
    _lookCenterY(0.5f)
{
    _executeAbsolutePath = "";
    _textureManager = new LAppTextureManager();
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
    const auto cursor = _compositor.TrackedCursor(std::chrono::steady_clock::now());
    if (!cursor) return; // pointer-on-surface events still work without it
    _cursorLook.GlobalPosition(cursor.value.first, cursor.value.second);
}

void LAppDelegate::UpdateCursorLook(ModelTransform transform)
{
    if (!_manager || !_cursorLook.HasPosition()) return;
    const auto outputs = _waylandBackend.Outputs();
    const auto* current = _outputController.Current(outputs);
    if (!current) return;
    const auto look = _cursorLook.LookAt(current->x, current->y, _windowWidth,
        _windowHeight, _lookCenterX, _lookCenterY, transform);
    _manager->OnDrag(look.first, look.second);
}

void LAppDelegate::OnMouseCallBack(void* window, int button, int action, int modify)
{
    if (_view == NULL)
    {
        return;
    }
    
    if (_settings && _settings->Editing())
    {
        _input = InputController{};
        if (button == 0 && _settings->ScreenEditing()) {
            if (action == 1) {
                _cropInteraction.Press(_settings->Display(), int(_mouseX), int(_mouseY));
            } else if (action == 0) {
                const auto result = _cropInteraction.Release(*_settings, ClickClock(), int(_mouseX), int(_mouseY));
                if (!result.ok) LAppPal::PrintLogLn("[Crop] Save failed: %s", result.error.c_str());
            }
        }
        return; // No model taps, skin changes or keyboard grabs during editing.
    }
    if (button == 0)
    {
        if (1 == action)
        {
            _input.Press(static_cast<int>(_mouseX), static_cast<int>(_mouseY));
            _view->OnTouchesBegan(_mouseX, _mouseY);
        }
        else if (0 == action)
        {
            if (_input.IsCaptured())
            {
                const bool moved = _input.DragX() != _input.StartX() || _input.DragY() != _input.StartY();
                // Execute screen switch on release
                const auto cursor = _compositor.GlobalCursor();
                bool switched = false;
                if (cursor) {
                    const auto geometry = _compositor.MonitorGeometry();
                    if (geometry) _waylandBackend.ApplyMonitorGeometry(geometry.value);
                    auto transform = _service.Snapshot().transform;
                    const OutputId oldId = _waylandBackend.CurrentOutput();
                    if (_outputController.MoveTo(cursor.value.first, cursor.value.second, transform) &&
                         oldId != _waylandBackend.CurrentOutput()) {
                         _service.SetTransform(transform);
                         _settings->PauseAutosave();
                         switched = true;
                    }
                }

                const auto result = _input.Release(ClickClock(), static_cast<int>(_mouseX),
                    static_cast<int>(_mouseY), [this](int x, int y)
                    {
                        _view->OnTouchesBegan(x, y);
                        _view->OnTouchesEnded(x, y);
                    });
                if (result == InputController::ReleaseResult::DoubleClick)
                {
                    _manager->OnDrag(0.0f, 0.0f);
                    SetModelKeyboardGrab(&_wlContext, true);
                }
                else if (result == InputController::ReleaseResult::Drag)
                {
                    _manager->OnDrag(0.0f, 0.0f); // End look
                }
                if (_settings && moved && !switched) _settings->ScheduleSave(SettingsClock(), .01);
            }
        }
    }
    else if (button == 1 && action == 0)
    {
        // Switch Models
        LAppPal::PrintLogLn("[Event] Switch Model Triggered");
          LAppPal::PrintLogLn("[Event] Switch Model Triggered");
          const auto state = _service.Snapshot();
          if (state.modelCount) _service.SelectModel((state.modelIndex + 1) % state.modelCount);
    }
    else if (button == 2 && action == 1)
    {
        // Switch Skin
        LAppPal::PrintLogLn("[Event] Switch Skin Triggered");
        _service.SwitchSkin();
    }
}

void LAppDelegate::FlushPendingTap()
{
    if (_settings && _settings->Editing()) return;
    _input.FlushTap([this](int x, int y)
    {
        _view->OnTouchesBegan(x, y);
        _view->OnTouchesEnded(x, y);
    });
}

void LAppDelegate::ResetKeyboardState()
{
    _input.ResetKeyboard();
}

void LAppDelegate::OnModelKey(uint32_t code, bool pressed)
{
    if (_settings && _settings->Editing()) return;
    LAppModel* model = _wlContext.keyboardSurface.IsGrabbed()
        ? _manager->GetModel(0) : nullptr;
    const auto* bindings = model ? &model->Hotkeys().Bindings() : nullptr;
    const ModelKeyDecision decision = _input.Key(
        _wlContext.keyboardSurface.IsGrabbed(), code, pressed, bindings);
    if (decision.action == ModelKeyDecision::ReleaseGrab)
        SetModelKeyboardGrab(&_wlContext, false);
    else if (decision.action == ModelKeyDecision::Trigger)
    {
        const auto result = _service.TriggerHotkey(decision.bindingId);
        if (!result.ok) LAppPal::PrintLogLn("[Hotkeys] Trigger failed for binding %s: %s",
            decision.bindingId.c_str(), result.error.c_str());
    }
}

void LAppDelegate::OnMouseCallBack(void* window, double x, double y)
{
    SetMousePosition(x, y);

    if (_view == NULL)
    {
        return;
    }

    if (_settings && _settings->Editing()) {
        if (_settings->ScreenEditing()) _cropInteraction.Move(*_settings, int(x), int(y), _windowHeight);
        return;
    }
    if (_input.IsCaptured())
    {
        if (_settings) {
            if (_cropInteraction.StartModelDrag(*_settings, _input.StartX(), _input.StartY(),
                    int(x), int(y), _windowWidth, _windowHeight)) {
                _input = InputController{};
                _manager->OnDrag(0.0f, 0.0f);
                if (_wlContext.keyboardSurface.IsGrabbed()) SetModelKeyboardGrab(&_wlContext, false);
            }
            return;
        }
        double curX = x;
        double curY = y;
        int deltaX = static_cast<int>(curX) - _input.DragX();
        int deltaY = static_cast<int>(curY) - _input.DragY();
        
        if (deltaX != 0 || deltaY != 0) {
            const auto next = InputController::Drag(_service.Snapshot().transform,
                deltaX, deltaY, _windowHeight);
            _service.SetTransform(next);
            
            _input.SetDragPosition(static_cast<int>(curX), static_cast<int>(curY));
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

void LAppDelegate::OnScrollCallBack(void* window, double xoffset, double yoffset)
{
    const ModelTransform transform = ZoomModelFromScroll(
        _service.Snapshot().transform, _view, _mouseX, _mouseY,
        static_cast<float>(yoffset));
    if (_settings && _settings->Editing()) {
        if (_settings->ScreenEditing()) { auto next = _settings->Display(); next.transform = transform; _settings->Update(_settings->Owner(), next); }
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
        if (!_wlContext.compositor || !_wlContext.surface) return false;
        if (!ClearWaylandInputRegion(&_wlContext)) return false;
        _hoverOpacity.Leave();
        SetModelKeyboardGrab(&_wlContext, false);
        _input.ResetKeyboard();
        return true;
    }
    return _wlContext.surface && _wlContext.compositor;
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
