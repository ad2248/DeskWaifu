/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "LAppLive2DManager.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <libgen.h>
#include <limits.h>
#include <string>
#include <utility>
#include <algorithm>
#include <GL/glew.h>

#include <Rendering/CubismRenderer.hpp>
#include <Rendering/OpenGL/CubismOffscreenManager_OpenGLES2.hpp>
#include "LAppPal.hpp"
#include "LAppDefine.hpp"
#include "LAppTextureManager.hpp"
#include "LAppModel.hpp"
#include "LAppSprite.hpp"
#include "ModelProjection.hpp"
#include "ModelCatalog.hpp"
#include "ModelRenderer.hpp"

using namespace Csm;
using namespace LAppDefine;

namespace {

    void BeganMotion(ACubismMotion* self)
    {
        LAppPal::PrintLogLn("Motion began: %x", self);
    }

    void FinishedMotion(ACubismMotion* self)
    {
        LAppPal::PrintLogLn("Motion Finished: %x", self);
    }

}

LAppLive2DManager::LAppLive2DManager(LAppTextureManager& textures, int width, int height,
    ModelDiscoveryOptions discovery)
    : _viewMatrix(NULL)
    , _textures(textures)
    , _viewportWidth(width)
    , _viewportHeight(height)
    , _discovery(std::move(discovery))
    , _loader(textures, width, height, _discovery.emotionTimeout)
{
    _viewMatrix = new CubismMatrix44();
    SetUpModel();

    if (_modelDir.GetSize() == 0)
    {
        LAppPal::PrintLogLn("[APP]No models found in: %s", _discovery.primaryDirectory.c_str());
        LAppPal::PrintLogLn("[APP]Please place your Live2D models in the models directory.");
        LAppPal::PrintLogLn("[APP]Each model should be in its own subfolder with a .model3.json file.");
        LAppPal::PrintLogLn("[APP]Example: %s<ModelName>/<ModelName>.model3.json", _discovery.primaryDirectory.c_str());
        return;
    }

    // 根据配置查找默认模型索引。
    csmInt32 selected = 0;
    const std::string& defaultModel = _discovery.defaultModel;
    if (!defaultModel.empty())
    {
        for (csmInt32 i = 0; i < _modelDir.GetSize(); i++)
        {
            if (strcmp(_modelDir[i].GetRawString(), defaultModel.c_str()) == 0)
            {
                selected = i;
                LAppPal::PrintLogLn("[APP]Default model set to: %s (index %d)", defaultModel.c_str(), i);
                break;
            }
        }
    }

    _session.Initialize(_modelDir.GetSize());
    ChangeScene(selected);
}

LAppLive2DManager::~LAppLive2DManager()
{
    ReleaseAllModel();
    delete _viewMatrix;
    Csm::Rendering::CubismOffscreenManager_OpenGLES2::ReleaseInstance();
}

void LAppLive2DManager::ReleaseAllModel()
{
    _models.Clear();
    _sampleModel.reset();
    _session.Clear();
}

void LAppLive2DManager::SetUpModel()
{
    _modelDir.Clear();

    std::vector<std::string> roots{_discovery.primaryDirectory};
    roots.insert(roots.end(), _discovery.additionalDirectories.begin(), _discovery.additionalDirectories.end());
    _descriptors = ModelCatalog::Scan(roots);
    for (const auto& descriptor : _descriptors)
    {
        _modelDir.PushBack(csmString(descriptor.name.c_str()));
    }
}

csmVector<csmString> LAppLive2DManager::GetModelDir() const
{
    return _modelDir;
}

csmInt32 LAppLive2DManager::GetModelDirSize() const
{
    return _modelDir.GetSize();
}

LAppModel* LAppLive2DManager::GetModel(csmUint32 no) const
{
    if (no < _models.GetSize())
    {
        return _models[no];
    }

    return NULL;
}

void LAppLive2DManager::SetRenderTargetSize(csmUint32 width, csmUint32 height)
{
    _viewportWidth = width;
    _viewportHeight = height;
    _loader.SetViewport(width, height);
    for (csmUint32 i = 0; i < _session.Cache().Size(); i++)
    {
        LAppModel* model = _session.Cache().Get(i);
        if (model) {
            model->SetViewport(width, height);
            model->SetRenderTargetSize(width, height);
        }
    }
}

void LAppLive2DManager::OnDrag(csmFloat32 x, csmFloat32 y) const
{
    for (csmUint32 i = 0; i < _models.GetSize(); i++)
    {
        LAppModel* model = GetModel(i);

        model->SetDragging(x, y);
    }
}

void LAppLive2DManager::OnTap(csmFloat32 clipX, csmFloat32 clipY, int width, int height, ModelTransform transform)
{
    if (width <= 0 || height <= 0) return;
    for (csmUint32 i = 0; i < _models.GetSize(); i++)
    {
        auto projection = BuildModelProjection(_models[i]->GetModel()->GetCanvasWidth(),
            width, height, transform, _viewMatrix);
        const float x = projection.InvertTransformX(clipX);
        const float y = projection.InvertTransformY(clipY);
        if (DebugLogEnable)
            LAppPal::PrintLogLn("[APP]tap point: {x:%.2f y:%.2f}", x, y);
        if (_models[i]->HitTest(HitAreaNameHead, x, y))
        {
            if (DebugLogEnable)
            {
                LAppPal::PrintLogLn("[APP]hit area: [%s]", HitAreaNameHead);
            }
            _models[i]->SetRandomExpression();
        }
        else if (_models[i]->HitTest(HitAreaNameBody, x, y))
        {
            if (DebugLogEnable)
            {
                LAppPal::PrintLogLn("[APP]hit area: [%s]", HitAreaNameBody);
            }
            _models[i]->StartRandomMotion(MotionGroupTapBody, PriorityNormal, FinishedMotion, BeganMotion);
            _models[i]->SetRandomExpression();
        }
    }
}

ModelHitSnapshot LAppLive2DManager::HitSnapshot(int width, int height, ModelTransform transform) const
{
    ModelHitSnapshot result; result.width = width; result.height = height;
    auto* model = GetModel(0);
    if (!model || width <= 0 || height <= 0) return result;
    result.modelToken = model->Hotkeys().Token();
    result.partsEnabled = model->GetOpacity() >= 1;
    auto projection = BuildModelProjection(model->GetModel()->GetCanvasWidth(), width, height, transform, _viewMatrix);
    auto* matrix = model->GetModelMatrix();
    const float x0 = matrix->InvertTransformX(projection.InvertTransformX(-1));
    const float y0 = matrix->InvertTransformY(projection.InvertTransformY(1));
    result.xOffset = x0; result.yOffset = y0;
    result.xScale = (matrix->InvertTransformX(projection.InvertTransformX(1)) - x0) / width;
    result.yScale = (matrix->InvertTransformY(projection.InvertTransformY(-1)) - y0) / height;
    result.fallbackYOffset = projection.InvertTransformY(1);
    result.fallbackYScale = (projection.InvertTransformY(-1) - result.fallbackYOffset) / height;
    auto* setting = model->GetModelSetting();
    result.fallback = setting->GetHitAreasCount() == 0;
    for (int i = 0; i < setting->GetHitAreasCount(); ++i) {
        const auto index = model->GetModel()->GetDrawableIndex(setting->GetHitAreaId(i));
        if (index < 0) continue;
        const auto count = model->GetModel()->GetDrawableVertexCount(index);
        const auto* vertices = model->GetModel()->GetDrawableVertices(index);
        if (count <= 0) continue;
        ModelHitArea area{setting->GetHitAreaName(i), vertices[0], vertices[1], vertices[0], vertices[1]};
        for (int j = 1; j < count; ++j) {
            area.left = std::min(area.left, vertices[j * 2]); area.right = std::max(area.right, vertices[j * 2]);
            area.top = std::min(area.top, vertices[j * 2 + 1]); area.bottom = std::max(area.bottom, vertices[j * 2 + 1]);
        }
        result.areas.push_back(std::move(area));
    }
    return result;
}

void LAppLive2DManager::OnUpdate(int width, int height, ModelTransform transform, const ModelFrameInput& input,
    const std::function<void(LAppModel&)>& beforeDraw,
    const std::function<void(LAppModel&)>& afterDraw) const
{
    ModelRenderer::Draw(_models, _viewMatrix, width, height, transform, input, beforeDraw, afterDraw);
}

bool LAppLive2DManager::NextScene()
{
    if (GetModelDirSize() == 0) return false;
    csmInt32 no = (_session.CurrentIndex() + 1) % GetModelDirSize();
    return ChangeScene(no);
}

void LAppLive2DManager::SwitchSkin()
{
    for (csmUint32 i = 0; i < _models.GetSize(); i++)
    {
        _models[i]->SwitchSkin();
    }
}

bool LAppLive2DManager::ChangeScene(Csm::csmInt32 index)
{
    if (GetModelDirSize() == 0 || index < 0 || index >= GetModelDirSize()) return false;
    if (!_session.Select(index, [this](int candidate)
        { return _loader.Load(_descriptors[candidate]); })) return false;
    // 更新显示模型列表。
    _models.Clear();
    _sampleModel.reset();
    _models.PushBack(_session.Current());
    // 缓存模型不能保留热键开关状态，也不能接受旧控制面板令牌。
    _session.Current()->Hotkeys().Reset();

#if defined(USE_RENDER_TARGET) || defined(USE_MODEL_RENDER_TARGET)
    // 第二个示例模型借用所选模型的资源描述。
    _sampleModel = _loader.Load(_descriptors[index]);
    if (_sampleModel)
    {
        _models.PushBack(_sampleModel.get());
        _sampleModel->GetModelMatrix()->TranslateX(0.2f);
    }
#endif
    return true;
}

csmUint32 LAppLive2DManager::GetModelNum() const
{
    return _models.GetSize();
}

void LAppLive2DManager::SetViewMatrix(CubismMatrix44* m)
{
    for (int i = 0; i < 16; i++) {
        _viewMatrix->GetArray()[i] = m->GetArray()[i];
    }
}
