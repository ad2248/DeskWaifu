#include <GL/glew.h>
#include "ModelRuntime.hpp"
#include "render/ModelRenderTarget.hpp"
#include "LAppDefine.hpp"
#include "LAppLive2DManager.hpp"
#include "LAppModel.hpp"
#include "LAppPal.hpp"
#include "LAppTextureManager.hpp"
#include "LAppView.hpp"
#include "ModelFrameRegion.hpp"
#include "ModelInputRegion.hpp"
#include "contracts/JsonHelpers.hpp"
#include <algorithm>
#include <stdexcept>

namespace {
QJsonObject Error(const std::string& error) { return {{"ok", false}, {"error", QString::fromStdString(error)}}; }
QJsonObject Ok() { return {{"ok", true}}; }
}

ModelRuntime::ModelRuntime() = default;

bool ModelRuntime::Initialize(const ModelRuntimeOptions& options, int width, int height)
{
    LAppPal::SetResourceBasePath(options.resourceBase);
    _option.LogFunction = LAppPal::PrintMessage;
    _option.LoggingLevel = LAppDefine::CubismLoggingLevel;
    _option.LoadFileFunction = LAppPal::LoadFileAsBytes;
    _option.ReleaseBytesFunction = LAppPal::ReleaseBytes;
    if (!Csm::CubismFramework::StartUp(&_allocator, &_option)) return false;
    _started = true;
    Csm::CubismFramework::Initialize();
    _textures.reset(new LAppTextureManager);
    _manager.reset(new LAppLive2DManager(*_textures, width, height,
        {options.primaryDirectory, options.additionalDirectories, options.defaultModel, options.emotionTimeout}));
    if (!_manager->HasModels()) return false;
    _view.reset(new LAppView(*_manager, *_textures));
    _view->Initialize(width, height); _view->InitializeSprite();
    _width = width; _height = height;
    glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    LAppPal::UpdateTime();
    return true;
}

ModelRuntime::~ModelRuntime() { Shutdown(false); }

bool ModelRuntime::Shutdown(bool contextCurrent)
{
    if (!_started) return true;
    if (!contextCurrent && (_view || _manager || _textures)) {
        // 不能在其他上下文或线程中执行 GL 析构。
        (void)_view.release(); (void)_manager.release(); (void)_textures.release();
        _started = false;
        return false;
    }
    _view.reset(); _manager.reset(); _textures.reset();
    Csm::CubismFramework::Dispose(); Csm::CubismFramework::CleanUp();
    _started = false;
    return true;
}

void ModelRuntime::Resize(int width, int height)
{
    if (width == _width && height == _height) return;
    _view->Initialize(width, height); _view->ResizeSprite(); _view->DestroySpriteRenderTarget();
    _manager->SetRenderTargetSize(width, height);
    _width = width; _height = height;
}

ModelMetadata ModelRuntime::Metadata() const
{
    ModelMetadata snapshot;
    if (!_manager) return snapshot;
    snapshot.index = _manager->GetSceneIndex();
    const auto names = _manager->GetModelDir();
    for (Csm::csmUint32 i = 0; i < names.GetSize(); ++i) snapshot.names.append(QString::fromUtf8(names[i].GetRawString()));
    auto* model = _manager->GetModel(0);
    if (!model) return snapshot;
    const auto& hotkeys = model->Hotkeys();
    snapshot.bindings = hotkeys.Bindings();
    QJsonArray bindings;
    for (const auto& binding : snapshot.bindings) {
        QJsonArray keys; for (const auto& key : binding.keys) keys.append(QString::fromStdString(key));
        bindings.append(QJsonObject{{"id", QString::fromStdString(binding.id)}, {"name", QString::fromStdString(binding.name)},
            {"action", QString::fromStdString(binding.action)}, {"file", QString::fromStdString(binding.file)},
            {"reason", QString::fromStdString(binding.reason)}, {"enabled", binding.enabled},
            {"active", binding.reason.empty() && hotkeys.IsActive(binding.id)}, {"keys", keys}});
    }
    snapshot.hotkeys = {{"ok", true}, {"model_token", QString::fromStdString(hotkeys.Token())},
        {"warning", QString::fromStdString(hotkeys.Error())}, {"hotkeys", bindings}};
    for (const auto& expression : model->GetExpressionIds()) snapshot.expressions.append(QString::fromStdString(expression));
    for (const auto& motion : model->GetMotionList()) snapshot.motions.append(QJsonObject{
        {"group", QString::fromStdString(motion.group)}, {"index", motion.index}, {"file", QString::fromStdString(motion.file)}});
    return snapshot;
}

QJsonObject ModelRuntime::Execute(const std::string& json)
try {
    ConfigStore::Snapshot request; std::string error;
    if (!ConfigStore::Parse(json, request, error, false)) return Error("invalid model operation JSON");
    const auto command = JsonGetString(request, "command");
    auto* model = _manager ? _manager->GetModel(0) : nullptr;
    if (!model) return Error("no model loaded");
    if (command == "set_model" || command == "next_model" || command == "prev_model") {
        int index = _manager->GetSceneIndex(), count = _manager->GetModelDirSize();
        if (command == "next_model") index = (index + 1) % count;
        else if (command == "prev_model") index = (index - 1 + count) % count;
        else {
            const auto name = JsonGetString(request, "name");
            index = JsonGetInt(request, "index", -1);
            if (!name.empty()) {
                const auto names = _manager->GetModelDir(); index = -1;
                for (Csm::csmUint32 i = 0; i < names.GetSize(); ++i) if (name == names[i].GetRawString()) index = int(i);
                if (index < 0) return Error("model not found");
            }
        }
        if (index < 0 || index >= count) return Error("invalid index");
        return _manager->ChangeScene(index) ? Ok() : Error("model load failed");
    }
    const auto token = JsonGetString(request, "model_token");
    if (!token.empty() && token != model->Hotkeys().Token()) return Error("Model changed; stale operation discarded");
    if (command == "trigger_hotkey") {
        if (JsonGetString(request, "model_token") != model->Hotkeys().Token()) return Error("Model changed; refresh hotkeys before triggering");
        if (!model->Hotkeys().Trigger(JsonGetString(request, "id"), error)) return Error(error.empty() ? "hotkey unavailable" : error);
        return Metadata().hotkeys;
    }
    if (command == "get_hotkeys") return Metadata().hotkeys;
    if (command == "set_expression") {
        const auto id = JsonGetString(request, "id");
        if (id.empty()) return Error("missing 'id'");
        const auto expressions = model->GetExpressionIds();
        if (std::find(expressions.begin(), expressions.end(), id) == expressions.end()) return Error("expression unavailable");
        model->SetExpression(id.c_str()); return Ok();
    }
    if (command == "do_motion") {
        const auto group = JsonGetString(request, "group");
        const int index = JsonGetInt(request, "index", 0), priority = JsonGetInt(request, "priority", LAppDefine::PriorityNormal);
        if (group.empty()) return model->StartRandomMotion(LAppDefine::MotionGroupTapBody, priority) != Csm::InvalidMotionQueueEntryHandleValue ? Ok() : Error("motion unavailable");
        for (const auto& motion : model->GetMotionList()) if (motion.group == group && motion.index == index)
            return model->StartMotion(group.c_str(), index, priority) != Csm::InvalidMotionQueueEntryHandleValue ? Ok() : Error("motion unavailable");
        return Error("motion unavailable");
    }
    if (command == "switch_skin") { _manager->SwitchSkin(); return Ok(); }
    if (command == "reset_drag") { _manager->OnDrag(0, 0); return Ok(); }
    if (command == "random_expression") { model->SetRandomExpression(); return Ok(); }
    if (command == "random_motion") {
        const auto group = JsonGetString(request, "group");
        if (group.empty()) return Error("missing motion group");
        return model->StartRandomMotion(group.c_str(), LAppDefine::PriorityNormal) != Csm::InvalidMotionQueueEntryHandleValue
            ? Ok() : Error("motion unavailable");
    }
    if (command == "tap_model") {
        const auto token = JsonGetString(request, "model_token");
        if (!token.empty() && token != model->Hotkeys().Token()) return Error("Model changed; stale tap discarded");
        const int width = JsonGetInt(request, "width", _width), height = JsonGetInt(request, "height", _height);
        if (width <= 0 || height <= 0) return Error("invalid viewport");
        Resize(width, height);
        _view->OnTouchesEnded(JsonGetFloat(request, "mouse_x", 0), JsonGetFloat(request, "mouse_y", 0),
            {JsonGetFloat(request, "zoom", 1), JsonGetFloat(request, "x", 0), JsonGetFloat(request, "y", 0)});
        return Ok();
    }
    return Error("unknown model operation");
} catch (const std::exception& error) { return Error(error.what()); }

std::shared_ptr<RenderFrame> ModelRuntime::Render(const ModelRenderState& state, ModelRenderTarget& target)
{
    if (!target.Bind(state.width, state.height)) throw std::runtime_error("cannot bind render-thread framebuffer");
    Resize(state.width, state.height);
    glViewport(0, 0, state.width, state.height);
    glClearColor(0, 0, 0, 0); glClearDepth(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    LAppPal::UpdateTime();
    const bool opacityFrame = state.opacity != 1 && !state.app.hidden && _view->BeginOpacityFrame();
    if (!state.app.hidden) {
        if (!state.editing && state.hasLook) _manager->OnDrag(state.lookX, state.lookY);
        // 切换模型后，即使短暂一帧也不能应用上一个模型的控制来源。
        auto* model = _manager->GetModel(0);
        _view->Render(state.app.transform, model && state.modelToken == model->Hotkeys().Token() ? state.controls : ModelFrameInput{});
    }
    auto frame = std::make_shared<RenderFrame>(); frame->width = state.width; frame->height = state.height;
    frame->modelIndex = _manager->GetSceneIndex(); frame->hidden = state.app.hidden;
    if (!state.app.hidden || state.editing) {
        frame->regions = ReadModelInputRegion(opacityFrame ? _view->OpacityFramebuffer() : target.Framebuffer(), state.width, state.height, state.clip, _inputPixels);
        if (opacityFrame) _view->PresentOpacityFrame(state.opacity);
        ClipModelRegion(state.clip, state.width, state.height);
        if (state.editing) DrawModelEditFrame(state.handles, state.width, state.height);
    }
    if (!target.Read(frame->rgba)) throw std::runtime_error("cannot read render-thread framebuffer");
    auto hit = std::make_shared<ModelHitSnapshot>(_manager->HitSnapshot(state.width, state.height, state.app.transform));
    hit->hidden = state.app.hidden;
    hit->regions = frame->regions;
    frame->hit = std::move(hit);
    return frame;
}
