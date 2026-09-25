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
#include <GL/glew.h>

#include <Rendering/CubismRenderer.hpp>
#include <Rendering/OpenGL/CubismOffscreenManager_OpenGLES2.hpp>
#include "LAppPal.hpp"
#include "LAppDefine.hpp"
#include "LAppTextureManager.hpp"
#include "LAppModel.hpp"
#include "LAppSprite.hpp"
#include "ModelProjection.hpp"

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

    int CompareCsmString(const void* a, const void* b)
    {
        return strcmp(reinterpret_cast<const Csm::csmString*>(a)->GetRawString(),
            reinterpret_cast<const Csm::csmString*>(b)->GetRawString());
    }
}

LAppLive2DManager::LAppLive2DManager(LAppTextureManager& textures, int width, int height,
    ModelDiscoveryOptions discovery)
    : _viewMatrix(NULL)
    , _textures(textures)
    , _viewportWidth(width)
    , _viewportHeight(height)
    , _discovery(std::move(discovery))
    , _sceneIndex(0)
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

    // Find default model index from config
    const std::string& defaultModel = _discovery.defaultModel;
    if (!defaultModel.empty())
    {
        for (csmInt32 i = 0; i < _modelDir.GetSize(); i++)
        {
            if (strcmp(_modelDir[i].GetRawString(), defaultModel.c_str()) == 0)
            {
                _sceneIndex = i;
                LAppPal::PrintLogLn("[APP]Default model set to: %s (index %d)", defaultModel.c_str(), i);
                break;
            }
        }
    }

    InitModelCache();

    ChangeScene(_sceneIndex);
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

    for (csmUint32 i = 0; i < _modelCache.GetSize(); i++)
    {
        if (_modelCache[i] != NULL)
        {
            delete _modelCache[i];
            _modelCache[i] = NULL;
        }
    }
    _modelCache.Clear();
}

void LAppLive2DManager::ScanModelsInDir(const csmString& basePath)
{
    struct dirent *entry;
    DIR *pDir = opendir(basePath.GetRawString());
    if (pDir == NULL)
    {
        LAppPal::PrintLogLn("[APP]Cannot open model directory: %s", basePath.GetRawString());
        return;
    }

    while ((entry = readdir(pDir)) != NULL)
    {
        if ((entry->d_type & DT_DIR) && strcmp(entry->d_name, "..") != 0 && strcmp(entry->d_name, ".") != 0)
        {
            struct dirent *entry2;
            csmString modelName(entry->d_name);

            csmString modelPath(basePath);
            modelPath += modelName;
            modelPath.Append(1, '/');

            DIR *pDir2 = opendir(modelPath.GetRawString());
            if (pDir2 == NULL) continue;

            while ((entry2 = readdir(pDir2)) != NULL)
            {
                const char* name = entry2->d_name;
                size_t len = strlen(name);
                const char* suffix = ".model3.json";
                size_t suffixLen = strlen(suffix);
                if (len > suffixLen && strcmp(name + len - suffixLen, suffix) == 0)
                {
                    _modelDir.PushBack(csmString(entry->d_name));
                    _modelBasePath.PushBack(basePath);
                    _modelJsonName.PushBack(csmString(name));
                    break;
                }
            }
            closedir(pDir2);
        }
    }
    closedir(pDir);
}

void LAppLive2DManager::SetUpModel()
{
    _modelDir.Clear();
    _modelBasePath.Clear();
    _modelJsonName.Clear();

    // Scan default models directory
    csmString defaultPath(_discovery.primaryDirectory.c_str());
    ScanModelsInDir(defaultPath);

    // Scan additional model directories from config
    for (size_t i = 0; i < _discovery.additionalDirectories.size(); i++)
    {
        std::string dir = _discovery.additionalDirectories[i];
        if (!dir.empty() && dir.back() != '/') dir += "/";

        // Resolve absolute path
        char resolved[PATH_MAX];
        if (realpath(dir.c_str(), resolved) != NULL)
        {
            dir = std::string(resolved) + "/";
        }

        csmString additionalPath(dir.c_str());
        LAppPal::PrintLogLn("[APP]Scanning additional model dir: %s", additionalPath.GetRawString());
        ScanModelsInDir(additionalPath);
    }

    // Sort model list (and keep base paths in sync) - simple bubble sort
    for (csmInt32 i = 0; i < (csmInt32)_modelDir.GetSize() - 1; i++)
    {
        for (csmInt32 j = 0; j < (csmInt32)_modelDir.GetSize() - 1 - i; j++)
        {
            if (strcmp(_modelDir[j].GetRawString(), _modelDir[j + 1].GetRawString()) > 0)
            {
                // Swap modelDir
                csmString tmpDir = _modelDir[j];
                _modelDir[j] = _modelDir[j + 1];
                _modelDir[j + 1] = tmpDir;
                // Swap basePath
                csmString tmpBase = _modelBasePath[j];
                _modelBasePath[j] = _modelBasePath[j + 1];
                _modelBasePath[j + 1] = tmpBase;
                // Swap jsonName
                csmString tmpJson = _modelJsonName[j];
                _modelJsonName[j] = _modelJsonName[j + 1];
                _modelJsonName[j + 1] = tmpJson;
            }
        }
    }
}

void LAppLive2DManager::InitModelCache()
{
    for (csmUint32 i = 0; i < _modelCache.GetSize(); i++)
    {
        if (_modelCache[i] != NULL)
        {
            delete _modelCache[i];
        }
    }
    _modelCache.Clear();

    for (csmInt32 i = 0; i < _modelDir.GetSize(); i++)
    {
        _modelCache.PushBack(NULL);
    }
}

void LAppLive2DManager::PreloadNextModel()
{
    if (GetModelDirSize() <= 1) return;

    csmInt32 nextIndex = (_sceneIndex + 1) % GetModelDirSize();

    if (_modelCache[nextIndex] != NULL) return;

    const csmString& model = _modelDir[nextIndex];
    LAppPal::PrintLogLn("[APP]preloading model: %s", model.GetRawString());

    csmString modelPath(_modelBasePath[nextIndex]);
    modelPath += model;
    modelPath.Append(1, '/');

    const csmString& modelJsonName = _modelJsonName[nextIndex];

    _modelCache[nextIndex] = new LAppModel(_textures, _viewportWidth, _viewportHeight, _discovery.emotionTimeout);
    _modelCache[nextIndex]->LoadAssets(modelPath.GetRawString(), modelJsonName.GetRawString());
}

void LAppLive2DManager::EvictExcessModels()
{
    if (GetModelDirSize() <= 4) return;

    csmInt32 nextIndex = (_sceneIndex + 1) % GetModelDirSize();

    for (csmInt32 i = 0; i < (csmInt32)_modelCache.GetSize(); i++)
    {
        if (i != _sceneIndex && i != nextIndex && _modelCache[i] != NULL)
        {
            LAppPal::PrintLogLn("[APP]evicting cached model: %s", _modelDir[i].GetRawString());
            delete _modelCache[i];
            _modelCache[i] = NULL;
        }
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
    for (csmUint32 i = 0; i < _modelCache.GetSize(); i++)
    {
        LAppModel* model = _modelCache[i];
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

void LAppLive2DManager::OnTap(csmFloat32 x, csmFloat32 y)
{
    if (DebugLogEnable)
    {
        LAppPal::PrintLogLn("[APP]tap point: {x:%.2f y:%.2f}", x, y);
    }

    for (csmUint32 i = 0; i < _models.GetSize(); i++)
    {
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

void LAppLive2DManager::OnUpdate(int width, int height, ModelTransform transform, const ModelFrameInput& input,
    const std::function<void(LAppModel&)>& beforeDraw,
    const std::function<void(LAppModel&)>& afterDraw) const
{
    if (width <= 0 || height <= 0) return;

    // モデルで使用するオフスクリーン管理の開始処理
    Csm::Rendering::CubismOffscreenManager_OpenGLES2::GetInstance()->BeginFrameProcess();

    csmUint32 modelCount = _models.GetSize();
    for (csmUint32 i = 0; i < modelCount; ++i)
    {
        CubismMatrix44 projection;
        LAppModel* model = GetModel(i);

        if (model->GetModel() == NULL)
        {
            LAppPal::PrintLogLn("Failed to model->GetModel().");
            continue;
        }

        if (model->GetModel()->GetCanvasWidth() > 1.0f && width < height)
        {
            model->GetModelMatrix()->SetWidth(2.0f);
            projection.Scale(1.0f, static_cast<float>(width) / static_cast<float>(height));
        }
        else
        {
            projection.Scale(static_cast<float>(height) / static_cast<float>(width), 1.0f);
        }

        ApplyModelTransform(projection, transform);

        // 必要があればここで乗算
        if (_viewMatrix != NULL)
        {
            projection.MultiplyByMatrix(_viewMatrix);
        }

        beforeDraw(*model);

        model->Update(input);
        model->Draw(projection);///< 参照渡しなのでprojectionは変質する

        afterDraw(*model);
    }

    // モデルで使用するオフスクリーン管理の終了処理
    Csm::Rendering::CubismOffscreenManager_OpenGLES2::GetInstance()->EndFrameProcess();
    // もし余っているオフスクリーンのリソースを解放したい場合行う処理
    Csm::Rendering::CubismOffscreenManager_OpenGLES2::GetInstance()->ReleaseStaleRenderTextures();
}

void LAppLive2DManager::NextScene()
{
    if (GetModelDirSize() == 0) return;
    csmInt32 no = (_sceneIndex + 1) % GetModelDirSize();
    ChangeScene(no);
}

void LAppLive2DManager::SwitchSkin()
{
    for (csmUint32 i = 0; i < _models.GetSize(); i++)
    {
        _models[i]->SwitchSkin();
    }
}

void LAppLive2DManager::ChangeScene(Csm::csmInt32 index)
{
    if (GetModelDirSize() == 0 || index < 0 || index >= GetModelDirSize()) return;

    _sceneIndex = index;
    if (DebugLogEnable)
    {
        LAppPal::PrintLogLn("[APP]model index: %d", _sceneIndex);
    }

    // モデルがキャッシュにない場合は読み込む
    if (_modelCache[index] == NULL)
    {
        const csmString& model = _modelDir[index];
        LAppPal::PrintLogLn("[APP]loading model: %s", model.GetRawString());

        csmString modelPath(_modelBasePath[index]);
        modelPath += model;
        modelPath.Append(1, '/');

        const csmString& modelJsonName = _modelJsonName[index];

        _modelCache[index] = new LAppModel(_textures, _viewportWidth, _viewportHeight, _discovery.emotionTimeout);
        _modelCache[index]->LoadAssets(modelPath.GetRawString(), modelJsonName.GetRawString());
    }
    else
    {
        LAppPal::PrintLogLn("[APP]using cached model: %s", _modelDir[index].GetRawString());
    }

    // 表示モデルリストを更新
    _models.Clear();
    _models.PushBack(_modelCache[index]);
    // Cached models must not retain hotkey switches or accept an old panel token.
    _modelCache[index]->Hotkeys().Reset();

    // モデル数が4を超える場合、現在と次以外のキャッシュを解放
    EvictExcessModels();

    // 次のモデルを事前読み込み
    PreloadNextModel();

#if defined(USE_RENDER_TARGET) || defined(USE_MODEL_RENDER_TARGET)
    // Sample second model borrows the selected model's asset descriptor.
    csmString modelPath(_modelBasePath[index]);
    modelPath += _modelDir[index];
    modelPath.Append(1, '/');
    _models.PushBack(new LAppModel(_textures, _viewportWidth, _viewportHeight, _discovery.emotionTimeout));
    _models[1]->LoadAssets(modelPath.GetRawString(), _modelJsonName[index].GetRawString());
    _models[1]->GetModelMatrix()->TranslateX(0.2f);
#endif
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
