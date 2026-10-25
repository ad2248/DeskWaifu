/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "LAppDelegate.hpp"
#include "processor/speech/source/MiniMaxSpeechSource.hpp"
#include "processor/speech/audio/AudioPlayer.hpp"
#include "processor/speech/SpeechPlayer.hpp"
#include "OcrClient.hpp"
#include "processor/ProcessorRuntime.hpp"
#include "processor/ipc/IpcProcessor.hpp"
#include "processor/model/ModelProcessor.hpp"
#include "processor/command/CommandProcessor.hpp"
#include "processor/signal/SignalProcessor.hpp"
#include "processor/interaction/InteractionProcessor.hpp"
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

#include "LAppPal.hpp"
#include "ModelInputRegion.hpp"
#include "LAppDefine.hpp"
#include "LAppConfig.hpp"
#include "LAppIPC.hpp"
#include "AppCommandHandler.hpp"

using namespace std;
using namespace LAppDefine;

namespace {
    LAppDelegate* s_instance = NULL;
    double SettingsClock()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
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

    _windowWidth = RenderTargetWidth;
    _windowHeight = RenderTargetHeight;

    if (!_window->Initialize(RenderTargetWidth, RenderTargetHeight, *this)) {
        return false;
    }

    SetExecuteAbsolutePath();
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
    _models.reset(new ModelProcessor(_processors->Loop()));
    _models->SetModelChanged([this] { _service.ClearControls(); CancelInteraction(); });
    const auto device = qEnvironmentVariable("DESKWAIFU_RENDER_DEVICE");
    auto target = _window->RenderTarget(device.toStdString());
    if (!_models->Start(std::move(target), {_executeAbsolutePath, LAppDefine::ModelsDir,
            values.defaultModel, modelDirs, values.emotionTimeout}, _windowWidth, _windowHeight)) return false;
    _interaction.reset(new InteractionProcessor(_processors->Loop(), *_window, _windowState,
        _service, *_settings, *_models, [this] { return _models->Metadata(); },
        [this] { MoveScreenDrag(); },
        [this] { _processors->SpeakGreeting(QStringLiteral("姐姐，你好喵")); }));

    _signals.reset(new SignalProcessor(_processors->Loop(),
        [this] { _service.SetVisibility(_service.Snapshot().hidden); },
        [this] {
            const auto focused = _compositor.FocusedOutput();
            if (focused && !_settings->Editing()) {
                if (!_outputController.Focus(focused.value))
                    LAppPal::PrintLogLn("[Wayland] Focused output not available: %s", focused.value.c_str());
                else _settings->PauseAutosave();
            } else LAppPal::PrintLogLn("[Wayland] Focus capability unavailable (%d)", static_cast<int>(focused.status));
        }));
    if (!_signals->Available()) return false;

    // 初始化 IPC 套接字服务端。
    _ipc = new LAppIPC();
    _commands = new AppCommandHandler(*this);
    _commandProcessor.reset(new CommandProcessor(_processors->Loop(), *_commands));
    _ipc->SetCommandService(*_commandProcessor);
    if (!_ipc->Initialize()) {
        LAppPal::PrintLogLn("[IPC] Initialization failed; refusing to start without command endpoint");
        delete _ipc;
        _ipc = nullptr;
        _commandProcessor.reset();
        delete _commands;
        _commands = nullptr;
        return false;
    }

    _ipcProcessor.reset(new IpcProcessor(_processors->Loop(), *_ipc));
    return true;
}

OcrClient& LAppDelegate::Ocr() { return _processors->Ocr(); }

void LAppDelegate::AppEnd()
{
    // 关闭前停止交互生产者，不在退出命令的回复前继续排入模型操作。
    if (_interaction) _interaction->Stop();
    _windowState.Send(ModelWindowState::Event::Close);
}

bool LAppDelegate::Release()
{
    AppEnd();
    _signals.reset();
    _ipcProcessor.reset();
    const bool released = !_models || _models->Stop();
    _interaction.reset(); // AppEnd 已停止交互生产；模型线程收尾后再注销请求者。
    _models.reset(); // 销毁命令服务前，清除待派发的操作完成事件。
    delete _ipc; _ipc = nullptr;
    _commandProcessor.reset();
    delete _commands; _commands = nullptr;
    _processors.reset();
    _speechPlayer.reset();
    _audioPlayer.reset();
    _speechSource.reset();
    _kwinCursor.Stop();
    _window->Shutdown();
    if (released) _windowState.Send(ModelWindowState::Event::Released);
    return released;
}
bool LAppDelegate::Run()
{
    bool renderingOk = true;
    const bool ran = _window->Run([this, &renderingOk] {
        if (!_windowState.Running()) return false;
        if (_window->OutputNeedsRebind()) _settings->AbortScreenEdit();
        _window->ReconcileOutputs();
        if (_lastOutput != _window->CurrentOutput()) {
            _settings->AbortScreenEdit();
            _settings->PauseAutosave();
            _lastOutput = _window->CurrentOutput();
        }
        if (!_windowState.Running()) return false;
        int width = _window->Width();
        int height = _window->Height();

        if (width != _windowWidth || height != _windowHeight) {
            if (_settings->ScreenEditing()) _settings->AbortScreenEdit();
            _windowState.Send(ModelWindowState::Event::CancelGesture);
            CancelInteraction();
        }
        ResizeView(width, height);

        glViewport(0, 0, _windowWidth, _windowHeight);

        TrackGlobalCursor();
        _lastOutput = _window->CurrentOutput();

        if (_interaction->Captured() && _settings->Dirty()) _settings->ScheduleSave(SettingsClock(), .5);
        _settings->Tick(SettingsClock());

        if (!_models->Error().isEmpty()) {
            renderingOk = false;
            LAppPal::PrintLogLn("[Render] %s", _models->Error().toUtf8().constData());
            AppEnd(); return false;
        }
        ModelRenderState state;
        state.app = _service.Snapshot(); state.controls = _service.FrameInput();
        state.modelToken = _models->Metadata().hotkeys.value("model_token").toString().toStdString();
        state.width = _windowWidth; state.height = _windowHeight;
        state.editing = _settings->Editing();
        const auto& settings = _settings->Display();
        state.clip = ResolveModelRegion(settings.cropped, settings.region, _windowWidth, _windowHeight);
        if (_settings->ScreenEditing() && settings.cropped) state.handles = CropInteraction::Handles(settings.region);
        const auto outputs = _window->Outputs();
        const auto* current = _outputController.Current(outputs);
        _interaction->ApplyLook(state, current, _lookCenterX, _lookCenterY,
            LAppConfig::GetInstance().values.mouseLookRadius);
        auto frame = _models->Frame();
        const bool usable = frame && frame->width == _windowWidth && frame->height == _windowHeight
            && frame->hidden == state.app.hidden && frame->modelIndex == state.app.modelIndex;
        const auto regions = usable ? frame->regions : std::vector<ModelRegion>{};
        state.opacity = _hoverOpacity.Resolve(_hoverSettings->Enabled(), _hoverSettings->Opacity(),
            regions, state.handles, _hoverSettings->IdleOpacity());
        _models->SetState(state);
        glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (usable && (!state.app.hidden || _settings->ScreenEditing()) && !_window->UploadFrame(*frame)) {
            renderingOk = false; AppEnd(); return false;
        }
        _window->SetInputRegion(state.app.hidden && !_settings->ScreenEditing() ? std::vector<ModelRegion>{}
            : ModelFrameInputRegions(_settings->ScreenEditing(), _windowWidth, _windowHeight, regions));
        return _windowState.Running();
    });
    const bool released = Release();
    if (released) LAppPal::PrintLogLn("[APP]GPU resources released before window teardown");
    else LAppPal::PrintLogLn("[APP]Could not restore GPU context for teardown; resources left to process cleanup");
    LAppDelegate::ReleaseInstance();
    return ran && released && renderingOk;
}
LAppDelegate::LAppDelegate():
    _window(CreateDesktopWindow()),
    _outputController(*_window),
    _compositor(CompositorAdapter::Detect(), _compositorTransport),
    _ipc(nullptr),
    _commands(nullptr),
    _service(*this),
    
    _windowWidth(0),
    _windowHeight(0),
    _lookCenterX(0.5f),
    _lookCenterY(0.5f)
{
    _executeAbsolutePath = "";
}

LAppDelegate::~LAppDelegate()
{

}

void LAppDelegate::TrackGlobalCursor()
{
    const auto outputs = _window->Outputs();
    const auto* current = _outputController.Current(outputs);
    _kwinCursor.Poll(current ? current->name : "");
    double x = 0, y = 0;
    if (_interaction && _kwinCursor.LocalPosition(_windowWidth, _windowHeight, x, y))
        _interaction->CursorPosition(x, y);
}

void LAppDelegate::ResizeView(int width, int height)
{
    if ((_windowWidth == width && _windowHeight == height) || width <= 0 || height <= 0) return;
    _windowWidth = width;
    _windowHeight = height;
}

void LAppDelegate::MoveScreenDrag()
{
    const auto cursor = _compositor.GlobalCursor();
    if (!cursor) return; // 缺少此能力时仍可使用表面局部编辑。
    const auto outputs = _window->Outputs();
    const auto oldId = _window->CurrentOutput();
    const auto* next = OutputController::Find(outputs,
        OutputController::At(outputs, cursor.value.first, cursor.value.second, oldId));
    if (!next || next->id == oldId) return;

    auto draft = _settings->Display();
    if (!_outputController.MoveTo(cursor.value.first, cursor.value.second, draft.transform)) return;

    // 裁剪坐标属于原屏幕，不随模型移动。
    // 先使用目标屏幕的完整范围，使移动后的模型保持可见，
    // 用户可继续调整裁剪区域，再显式保存。
    draft.region = {0, 0, _window->Width(), _window->Height()};
    _settings->Update(_settings->Owner(), draft);
    _settings->PauseAutosave();
    // 下一帧不能将此次主动重绑定误判为热插拔或尺寸变化而取消编辑。
    // 编辑仍须显式保存，移动到其他显示器不会自动保存。
    _lastOutput = _window->CurrentOutput();
    ResizeView(_window->Width(), _window->Height());
    SetMousePosition(cursor.value.first - next->x, cursor.value.second - next->y);
}

void LAppDelegate::CancelInteraction()
{
    if (_interaction) _interaction->Cancel();
    else if (_window->KeyboardGrabbed()) _window->SetKeyboardGrab(false);
}

void LAppDelegate::SyncInteractionMode()
{
    if (!_settings || !_windowState.Running()) return;
    _windowState.Send(!_settings->Editing() ? ModelWindowState::Event::NormalMode :
        _settings->ScreenEditing() ? ModelWindowState::Event::ScreenMode : ModelWindowState::Event::PanelMode);
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
    if (_interaction) _interaction->PointerButton(button, pressed);
}

void LAppDelegate::ResetKeyboardState()
{
    if (_interaction) _interaction->ResetKeyboard();
}

void LAppDelegate::OnModelKey(uint32_t code, bool pressed)
{
    if (_interaction) _interaction->Key(code, pressed);
}

void LAppDelegate::PointerMoved(double x, double y)
{
    SetMousePosition(x, y);

    if (_interaction) _interaction->PointerMoved(x, y);
}

void LAppDelegate::SetMousePosition(double x, double y)
{
    _hoverOpacity.Move(x, y);
    if (_interaction) _interaction->Position(x, y);
}

void LAppDelegate::PointerScroll(double vertical)
{
    if (_interaction) _interaction->PointerScroll(vertical);
}

int LAppDelegate::ModelIndex() const { return _models ? _models->Metadata().index : -1; }
int LAppDelegate::ModelCount() const { return _models ? _models->Metadata().names.size() : 0; }

bool LAppDelegate::SetHidden(bool hidden)
{
    if (hidden) {
        if (!_window->SetInputRegion({})) return false;
        _windowState.Send(ModelWindowState::Event::CancelGesture);
        CancelInteraction();
        _hoverOpacity.Leave();
        _window->SetKeyboardGrab(false);
        ResetKeyboardState();
        return true;
    }
    return _window->Available();
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
