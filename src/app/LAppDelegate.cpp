#include "LAppDelegate.hpp"
#include "processor/interaction/config/LAppConfig.hpp"
#include "processor/model/LAppDefine.hpp"
#include "processor/ipc/LAppIPC.hpp"
#include "processor/model/ModelProcessor.hpp"
#include "processor/interaction/InteractionProcessor.hpp"
#include "processor/input/InputListener.hpp"
#include "processor/command/CommandProcessor.hpp"
#include "processor/ipc/IpcProcessor.hpp"
#include "processor/signal/SignalProcessor.hpp"
#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <cstdio>

namespace { LAppDelegate* instance = nullptr; }
LAppDelegate* LAppDelegate::GetInstance() { if (!instance) instance = new LAppDelegate; return instance; }
void LAppDelegate::ReleaseInstance() { delete instance; instance = nullptr; }
LAppDelegate::~LAppDelegate() { Release(); }

bool LAppDelegate::Initialize()
{
    const auto config = LAppConfig::GetInstance();
    _processors.reset(new ProcessorRuntime(QString::fromStdString(config.values.minimaxApiKey),
        [] { return QGuiApplication::clipboard()->image(); }));
    auto& loop = _processors->Loop();
    _exchange = std::make_shared<ModelExchange>();
    _models.reset(new ModelProcessor(loop, _exchange));
    _interaction.reset(new InteractionProcessor(loop, *_models, CreateDesktopWindow(), config, _exchange));
    _input.reset(new InputListener(loop, *_interaction));
    _commands.reset(new CommandProcessor(loop));
    if (!_interaction->Start(LAppDefine::RenderTargetWidth, LAppDefine::RenderTargetHeight)) return false;
    auto directories = config.values.additionalModelDirs;
    for (auto& directory : directories) directory = LAppConfig::ExpandTilde(directory);
    int width, height;
    { std::lock_guard<std::mutex> lock(_exchange->mutex); width = _exchange->state.width; height = _exchange->state.height; }
    if (!_models->Start(_interaction->RenderTarget(qEnvironmentVariable("DESKWAIFU_RENDER_DEVICE").toStdString()),
        {QCoreApplication::applicationDirPath().toStdString() + "/", LAppDefine::ModelsDir,
            config.values.defaultModel, directories, config.values.emotionTimeout}, width, height)) return false;
    if (!_input->StartCursor())
        std::fprintf(stderr, "[KWin] Global cursor unavailable: %s; using surface events\n", _input->CursorError().c_str());
    _signals.reset(new SignalProcessor(loop));
    if (!_signals->Available()) return false;
    _ipcProcessor.reset(new IpcProcessor(loop, std::unique_ptr<LAppIPC>(new LAppIPC), *_commands));
    if (!_ipcProcessor->Start()) return false;
    return true;
}

bool LAppDelegate::Release()
{
    // 先停止所有生产源，再取消/完成请求、join，最后注销与销毁。
    _signals.reset();
    if (_ipcProcessor) _ipcProcessor->Stop();
    if (_input) _input->Stop();
    if (_interaction) _interaction->Stop();
    if (_commands) _commands->Stop();
    const bool released = !_models || _models->Stop();
    _ipcProcessor.reset(); _commands.reset(); _input.reset(); _models.reset();
    if (_interaction) _interaction->ShutdownWindow();
    _interaction.reset();
    _processors.reset();
    return released;
}

bool LAppDelegate::Run()
{
    const bool ran = _interaction && _interaction->Run();
    const bool released = Release();
    if (released) std::printf("[APP]GPU resources released before window teardown\n");
    else std::fprintf(stderr, "[APP]Could not restore GPU context for teardown; resources left to process cleanup\n");
    ReleaseInstance();
    return ran && released;
}
