/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "LAppModel.hpp"
#include <dirent.h>
#include <cstring>
#include <stdlib.h>
#include <fstream>
#include <vector>
#include <CubismModelSettingJson.hpp>
#include <Motion/CubismMotion.hpp>
#include <Physics/CubismPhysics.hpp>
#include <CubismDefaultParameterId.hpp>
#include <Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp>
#include <Utils/CubismString.hpp>
#include <Id/CubismIdManager.hpp>
#include <Motion/CubismMotionQueueEntry.hpp>
#include "LAppDefine.hpp"
#include "LAppPal.hpp"
#include "LAppTextureManager.hpp"
#include <Motion/CubismMotionJson.hpp>
#include "Motion/CubismBreathUpdater.hpp"
#include "Motion/CubismLookUpdater.hpp"
#include "Motion/CubismExpressionUpdater.hpp"
#include "Motion/CubismEyeBlinkUpdater.hpp"
#include "Motion/CubismLipSyncUpdater.hpp"
#include "Motion/CubismPhysicsUpdater.hpp"
#include "Motion/CubismPoseUpdater.hpp"

using namespace Live2D::Cubism::Framework;
using namespace Live2D::Cubism::Framework::DefaultParameterId;
using namespace LAppDefine;

const csmFloat32 LAppModel::ExpressionTimeoutSeconds = 5.0f;

LAppModel::LAppModel(LAppTextureManager& textures, int width, int height, float emotionTimeout)
    : LAppModel_Common()
    , _textures(textures)
    , _viewportWidth(width)
    , _viewportHeight(height)
    , _emotionTimeout(emotionTimeout)
    , _modelSetting(NULL)
    , _userTimeSeconds(0.0f)
    , _motionUpdated(false)
    , _currentSkinIndex(0)
    , _lastExpressionTime(-1.0f)
    , _nextExpressionIndex(0)
{
    if (DebugLogEnable)
    {
        _debugMode = true;
    }

    _idParamAngleX = CubismFramework::GetIdManager()->GetId(ParamAngleX);
    _idParamAngleY = CubismFramework::GetIdManager()->GetId(ParamAngleY);
    _idParamAngleZ = CubismFramework::GetIdManager()->GetId(ParamAngleZ);
    _idParamBodyAngleX = CubismFramework::GetIdManager()->GetId(ParamBodyAngleX);
    _idParamBodyAngleY = CubismFramework::GetIdManager()->GetId(ParamBodyAngleY);
    _idParamBodyAngleZ = CubismFramework::GetIdManager()->GetId(ParamBodyAngleZ);
    _idParamEyeBallX = CubismFramework::GetIdManager()->GetId(ParamEyeBallX);
    _idParamEyeBallY = CubismFramework::GetIdManager()->GetId(ParamEyeBallY);
}

LAppModel::~LAppModel()
{
    _renderBuffer.DestroyRenderTarget();

    DeleteRenderer();
    _textureLeases.clear();

    ReleaseMotions();
    ReleaseExpressions();

    for (csmInt32 i = 0; _modelSetting && i < _modelSetting->GetMotionGroupCount(); i++)
    {
        const csmChar* group = _modelSetting->GetMotionGroupName(i);
        ReleaseMotionGroup(group);
    }
    delete(_modelSetting);
}

bool LAppModel::LoadAssets(const csmChar* dir, const csmChar* fileName)
{
    _modelHomeDir = dir;

    if (_debugMode)
    {
        LAppPal::PrintLogLn("[APP]load model setting: %s", fileName);
    }

    csmSizeInt size;
    const csmString path = csmString(dir) + fileName;

    csmByte* buffer = CreateBuffer(path.GetRawString(), &size);
    if (!buffer) return false;
    ICubismModelSetting* setting = new CubismModelSettingJson(buffer, size);
    DeleteBuffer(buffer, path.GetRawString());

    SetupModel(setting);
    _hotkeys.Load(dir, fileName);

    if (_model == NULL)
    {
        LAppPal::PrintLogLn("Failed to LoadAssets().");
        return false;
    }

    CreateRenderer(_viewportWidth, _viewportHeight);

    return SetupTextures();
}


void LAppModel::SetupModel(ICubismModelSetting* setting)
{
    _updating = true;
    _initialized = false;

    _modelSetting = setting;

    csmByte* buffer;
    csmSizeInt size;

    // Cubism 模型。
    if (strcmp(_modelSetting->GetModelFileName(), "") != 0)
    {
        csmString path = _modelSetting->GetModelFileName();
        path = _modelHomeDir + path;

        if (_debugMode)
        {
            LAppPal::PrintLogLn("[APP]create model: %s", setting->GetModelFileName());
        }

        buffer = CreateBuffer(path.GetRawString(), &size);
        if (!buffer) return;
        LoadModel(buffer, size);
        DeleteBuffer(buffer, path.GetRawString());
    }

    // 表情。
    {
        csmBool hasExpression = false;
        
        // 1. 从 model3.json 加载。
        if (_modelSetting->GetExpressionCount() > 0)
        {
            const csmInt32 count = _modelSetting->GetExpressionCount();
            for (csmInt32 i = 0; i < count; i++)
            {
                csmString name = _modelSetting->GetExpressionName(i);
                csmString path = _modelSetting->GetExpressionFileName(i);
                path = _modelHomeDir + path;

                buffer = CreateBuffer(path.GetRawString(), &size);
                ACubismMotion* motion = LoadExpression(buffer, size, name.GetRawString());

                if (motion)
                {
                    if (_expressions[name] != NULL)
                    {
                        ACubismMotion::Delete(_expressions[name]);
                        _expressions[name] = NULL;
                    }
                    _expressions[name] = motion;
                    hasExpression = true;
                }
                DeleteBuffer(buffer, path.GetRawString());
            }
        }
        
        // 2. 自动发现独立表情，包括 VTube Studio 的 exp/ 目录。
        for (const auto& directory : ModelHotkeys::ExpressionDirectories()) {
            csmString searchDir = _modelHomeDir + directory.c_str();
            DIR* pDir = opendir(searchDir.GetRawString());
            if (pDir != NULL) {
                struct dirent* ent;
                while ((ent = readdir(pDir)) != NULL) {
                    csmString fileName = ent->d_name;
                    if (strstr(fileName.GetRawString(), ".exp3.json")) {
                        csmString name = fileName;
                        if (_expressions[name] == NULL) { // 尚未加载。
                            csmString fullPath = searchDir + fileName;
                            buffer = CreateBuffer(fullPath.GetRawString(), &size);
                            if (buffer) {
                                ACubismMotion* motion = LoadExpression(buffer, size, name.GetRawString());
                                if (motion) {
                                    _expressions[name] = motion;
                                    hasExpression = true;
                                    LAppPal::PrintLogLn("[APP] Auto-loaded standalone expression: %s", name.GetRawString());
                                }
                                DeleteBuffer(buffer, fullPath.GetRawString());
                            }
                        }
                    }
                }
                closedir(pDir);
            }
        }

        if (hasExpression)
        {
            CubismExpressionUpdater* expression = CSM_NEW CubismExpressionUpdater(*_expressionManager);
            _updateScheduler.AddUpdatableList(expression);
        }
    }

    // 物理效果。
    if (strcmp(_modelSetting->GetPhysicsFileName(), "") != 0)
    {
        csmString path = _modelSetting->GetPhysicsFileName();
        path = _modelHomeDir + path;

        buffer = CreateBuffer(path.GetRawString(), &size);
        LoadPhysics(buffer, size);
        DeleteBuffer(buffer, path.GetRawString());

        if (_physics != nullptr)
        {
            CubismPhysicsUpdater* physics = CSM_NEW CubismPhysicsUpdater(*_physics);
            _updateScheduler.AddUpdatableList(physics);
        }
    }

    // 姿势。
    if (strcmp(_modelSetting->GetPoseFileName(), "") != 0)
    {
        csmString path = _modelSetting->GetPoseFileName();
        path = _modelHomeDir + path;

        buffer = CreateBuffer(path.GetRawString(), &size);
        LoadPose(buffer, size);
        DeleteBuffer(buffer, path.GetRawString());

        if (_pose != nullptr)
        {
            CubismPoseUpdater* pose = CSM_NEW CubismPoseUpdater(*_pose);
            _updateScheduler.AddUpdatableList(pose);
        }
    }

    // 眨眼。
    {
        if (_modelSetting->GetEyeBlinkParameterCount() > 0)
        {
            _eyeBlink = CubismEyeBlink::Create(_modelSetting);

            CubismEyeBlinkUpdater* eyeBlink = CSM_NEW CubismEyeBlinkUpdater(_motionUpdated, *_eyeBlink);
            _updateScheduler.AddUpdatableList(eyeBlink);
        }
    }

    // 呼吸。
    {
        _breath = CubismBreath::Create();

        csmVector<CubismBreath::BreathParameterData> breathParameters;

        breathParameters.PushBack(CubismBreath::BreathParameterData(_idParamAngleX, 0.0f, 15.0f, 6.5345f, 0.5f));
        breathParameters.PushBack(CubismBreath::BreathParameterData(_idParamAngleY, 0.0f, 8.0f, 3.5345f, 0.5f));
        breathParameters.PushBack(CubismBreath::BreathParameterData(_idParamAngleZ, 0.0f, 10.0f, 5.5345f, 0.5f));
        breathParameters.PushBack(CubismBreath::BreathParameterData(_idParamBodyAngleX, 0.0f, 4.0f, 15.5345f, 0.5f));
        breathParameters.PushBack(CubismBreath::BreathParameterData(CubismFramework::GetIdManager()->GetId(ParamBreath), 0.5f, 0.5f, 3.2345f, 0.5f));

        _breath->SetParameters(breathParameters);

        CubismBreathUpdater* breath = CSM_NEW CubismBreathUpdater(*_breath);
        _updateScheduler.AddUpdatableList(breath);
    }

    // 用户数据。
    if (strcmp(_modelSetting->GetUserDataFile(), "") != 0)
    {
        csmString path = _modelSetting->GetUserDataFile();
        path = _modelHomeDir + path;
        buffer = CreateBuffer(path.GetRawString(), &size);
        LoadUserData(buffer, size);
        DeleteBuffer(buffer, path.GetRawString());
    }

    // 眨眼参数 ID。
    {
        csmInt32 eyeBlinkIdCount = _modelSetting->GetEyeBlinkParameterCount();
        for (csmInt32 i = 0; i < eyeBlinkIdCount; ++i)
        {
            _eyeBlinkIds.PushBack(_modelSetting->GetEyeBlinkParameterId(i));
        }
    }

    // 唇形同步参数 ID。
    {
        csmInt32 lipSyncIdCount = _modelSetting->GetLipSyncParameterCount();
        for (csmInt32 i = 0; i < lipSyncIdCount; ++i)
        {
            _lipSyncIds.PushBack(_modelSetting->GetLipSyncParameterId(i));
        }

        CubismLipSyncUpdater* lipSync = CSM_NEW CubismLipSyncUpdater(_lipSyncIds, _wavFileHandler);
        _updateScheduler.AddUpdatableList(lipSync);
    }

    // 视线。
    {
        _look = CubismLook::Create();

        csmVector<CubismLook::LookParameterData> lookParameters;

        lookParameters.PushBack(CubismLook::LookParameterData(_idParamAngleX, 30.0f));
        lookParameters.PushBack(CubismLook::LookParameterData(_idParamAngleY, 0.0f, 30.0f));
        lookParameters.PushBack(CubismLook::LookParameterData(_idParamAngleZ, 0.0f, 0.0f, -30.0f));
        lookParameters.PushBack(CubismLook::LookParameterData(_idParamBodyAngleX, 10.0f));
        lookParameters.PushBack(CubismLook::LookParameterData(_idParamBodyAngleY, 0.0f, 10.0f));
        lookParameters.PushBack(CubismLook::LookParameterData(_idParamBodyAngleZ, 0.0f, 0.0f, -10.0f));
        lookParameters.PushBack(CubismLook::LookParameterData(_idParamEyeBallX, 1.0f));
        lookParameters.PushBack(CubismLook::LookParameterData(_idParamEyeBallY, 0.0f, 1.0f));

        _look->SetParameters(lookParameters);

        CubismLookUpdater* look = CSM_NEW CubismLookUpdater(*_look, *_dragManager);
        _updateScheduler.AddUpdatableList(look);
    }

    _updateScheduler.SortUpdatableList();

    if (_modelSetting == NULL || _modelMatrix == NULL)
    {
        LAppPal::PrintLogLn("Failed to SetupModel().");
        return;
    }

    // 布局。
    csmMap<csmString, csmFloat32> layout;
    _modelSetting->GetLayoutMap(layout);
    _modelMatrix->SetupFromLayout(layout);

    _model->SaveParameters();

    for (csmInt32 i = 0; i < _modelSetting->GetMotionGroupCount(); i++)
    {
        const csmChar* group = _modelSetting->GetMotionGroupName(i);
        PreloadMotionGroup(group);
    }

    // 自动发现额外的 .motion3.json 文件。
    DIR* pDirM = opendir(_modelHomeDir.GetRawString());
    if (pDirM != NULL) {
        struct dirent* entM;
        while ((entM = readdir(pDirM)) != NULL) {
            csmString fileName = entM->d_name;
            if (strstr(fileName.GetRawString(), ".motion3.json") && !strstr(fileName.GetRawString(), "loop") && !strstr(fileName.GetRawString(), "idle")) {
                // 很难准确判断文件是否已属于某个动作组，因此直接加载。
                csmString fullPath = _modelHomeDir + fileName;
                csmSizeInt sizeM;
                csmByte* buffM = CreateBuffer(fullPath.GetRawString(), &sizeM);
                if (buffM) {
                    ACubismMotion* motion = LoadMotion(buffM, sizeM, fileName.GetRawString());
                    if (motion) {
                        _autoMotions.PushBack(motion);
                        LAppPal::PrintLogLn("[APP] Auto-loaded standalone motion: %s", fileName.GetRawString());
                    }
                    DeleteBuffer(buffM, fullPath.GetRawString());
                }
            }
        }
        closedir(pDirM);
    }

    _motionManager->StopAllMotions();

    // 收集各皮肤动作组使用的全部参数 ID。
    CollectSkinParams();

    _updating = false;
    _initialized = true;
}

void LAppModel::CollectSkinParams()
{
    _allSkinParamIds.Clear();
    _allSkinParamDefaults.Clear();

    csmInt32 groupCount = _modelSetting->GetMotionGroupCount();
    if (groupCount <= 1) return; // 无需切换皮肤。

    // 收集所有动作组中参与动画的参数 ID，并去重。
    for (csmInt32 g = 0; g < groupCount; g++)
    {
        const csmChar* group = _modelSetting->GetMotionGroupName(g);
        csmInt32 motionCount = _modelSetting->GetMotionCount(group);
        for (csmInt32 m = 0; m < motionCount; m++)
        {
            csmString path = _modelSetting->GetMotionFileName(group, m);
            path = _modelHomeDir + path;

            csmSizeInt size;
            csmByte* buffer = CreateBuffer(path.GetRawString(), &size);
            if (!buffer) continue;

            CubismMotionJson motionJson(buffer, size);
            csmInt32 curveCount = motionJson.GetMotionCurveCount();
            for (csmInt32 c = 0; c < curveCount; c++)
            {
                const csmChar* target = motionJson.GetMotionCurveTarget(c);
                if (strcmp(target, "Parameter") != 0) continue;

                CubismIdHandle paramId = motionJson.GetMotionCurveId(c);

                // 检查是否已经收集。
                bool found = false;
                for (csmInt32 k = 0; k < _allSkinParamIds.GetSize(); k++)
                {
                    if (_allSkinParamIds[k] == paramId)
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    _allSkinParamIds.PushBack(paramId);
                    // 保存此参数在模型中的默认值。
                    csmInt32 paramIndex = _model->GetParameterIndex(paramId);
                    csmFloat32 defaultVal = _model->GetParameterDefaultValue(paramIndex);
                    _allSkinParamDefaults.PushBack(defaultVal);
                    LAppPal::PrintLogLn("[APP] Collected skin param: %s (default=%.2f)",
                        paramId->GetString().GetRawString(), defaultVal);
                }
            }

            DeleteBuffer(buffer, path.GetRawString());
        }
    }

    LAppPal::PrintLogLn("[APP] Total skin params collected: %d", _allSkinParamIds.GetSize());
}

void LAppModel::PreloadMotionGroup(const csmChar* group)
{
    const csmInt32 count = _modelSetting->GetMotionCount(group);

    for (csmInt32 i = 0; i < count; i++)
    {
        // 例如 idle_0。
        csmString name = Utils::CubismString::GetFormatedString("%s_%d", group, i);
        csmString path = _modelSetting->GetMotionFileName(group, i);
        path = _modelHomeDir + path;

        if (_debugMode)
        {
            LAppPal::PrintLogLn("[APP]load motion: %s => [%s_%d] ", path.GetRawString(), group, i);
        }

        csmByte* buffer;
        csmSizeInt size;
        buffer = CreateBuffer(path.GetRawString(), &size);
        CubismMotion* tmpMotion = static_cast<CubismMotion*>(LoadMotion(buffer, size, name.GetRawString(), NULL, NULL, _modelSetting, group, i));

        if (tmpMotion)
        {
            tmpMotion->SetEffectIds(_eyeBlinkIds, _lipSyncIds);

            if (_motions[name] != NULL)
            {
                ACubismMotion::Delete(_motions[name]);
            }
            _motions[name] = tmpMotion;
        }

        DeleteBuffer(buffer, path.GetRawString());
    }
}

void LAppModel::ReleaseMotionGroup(const csmChar* group) const
{
    const csmInt32 count = _modelSetting->GetMotionCount(group);
    for (csmInt32 i = 0; i < count; i++)
    {
        csmString voice = _modelSetting->GetMotionSoundFileName(group, i);
        if (strcmp(voice.GetRawString(), "") != 0)
        {
            csmString path = voice;
            path = _modelHomeDir + path;
        }
    }
}

/**
* @brief 释放全部动作数据。
*
* 释放全部动作数据。
*/
void LAppModel::ReleaseMotions()
{
    for (csmMap<csmString, ACubismMotion*>::const_iterator iter = _motions.Begin(); iter != _motions.End(); ++iter)
    {
        ACubismMotion::Delete(iter->Second);
    }

    _motions.Clear();
    for (csmUint32 i=0; i<_autoMotions.GetSize(); ++i) { ACubismMotion::Delete(_autoMotions[i]); }
    _autoMotions.Clear();
}

/**
* @brief 释放全部表情数据。
*
* 释放全部表情数据。
*/
void LAppModel::ReleaseExpressions()
{
    for (csmMap<csmString, ACubismMotion*>::const_iterator iter = _expressions.Begin(); iter != _expressions.End(); ++iter)
    {
        ACubismMotion::Delete(iter->Second);
    }

    _expressions.Clear();
}

void LAppModel::Update(const ModelFrameInput& input)
{
    const csmFloat32 deltaTimeSeconds = LAppPal::GetDeltaTime();
    _userTimeSeconds += deltaTimeSeconds;

    // 动作是否更新了参数。
    _motionUpdated = false;

    //-----------------------------------------------------------------
    _model->LoadParameters(); // 加载上次保存的状态。

    if (_motionManager->IsFinished())
    {
        // 没有正在播放的动作时，随机播放待机动作。
        StartRandomMotion(MotionGroupIdle, PriorityIdle);
    }
    else
    {
        _motionUpdated = _motionManager->UpdateMotion(_model, deltaTimeSeconds); // 更新动作。
    }
    _model->SaveParameters(); // 保存状态。
    //-----------------------------------------------------------------

    // 不透明度
    _opacity = _model->GetModelOpacity();

    // 在 CubismLookUpdater 使用鼠标目标前应用覆盖值；若在调度器之后应用，
    // 下一帧的鼠标更新会将其覆盖。
    if (input.hasLook) _dragManager->Set(input.lookX, input.lookY);
    _updateScheduler.OnLateUpdate(_model, deltaTimeSeconds);

    // 临时表情流程结束后，再应用持久热键表情。
    // 上面的 LoadParameters 每帧恢复基础值，因此 Add 不会逐帧累积。
    _hotkeys.Update(deltaTimeSeconds);
    _hotkeys.Apply(_model);

    // 外部嘴部 Y 覆盖值，用于 IPC 唇形同步。
    if (input.hasMouth)
    {
        for (csmUint32 i = 0; i < _lipSyncIds.GetSize(); ++i)
            _model->SetParameterValue(_lipSyncIds[i], input.mouth);
    }

    // 表情超时：经过 emotion_timeout 秒后恢复默认表情。
    // emotion_timeout < 0 表示不自动恢复。
    {
        float timeout = _emotionTimeout;
        if (timeout >= 0.0f && _lastExpressionTime >= 0.0f &&
            (_userTimeSeconds - _lastExpressionTime) > timeout)
        {
            _expressionManager->StopAllMotions();
            _lastExpressionTime = -1.0f;
            if (_debugMode) LAppPal::PrintLogLn("[APP] Expression timed out, reverted to default");
        }
    }

    _model->Update();

}

CubismMotionQueueEntryHandle LAppModel::StartMotion(const csmChar* group, csmInt32 no, csmInt32 priority, ACubismMotion::FinishedMotionCallback onFinishedMotionHandler, ACubismMotion::BeganMotionCallback onBeganMotionHandler)
{
    if (priority == PriorityForce)
    {
        _motionManager->SetReservePriority(priority);
    }
    else if (!_motionManager->ReserveMotion(priority))
    {
        if (_debugMode)
        {
            LAppPal::PrintLogLn("[APP]can't start motion.");
        }
        return InvalidMotionQueueEntryHandleValue;
    }

    const csmString fileName = _modelSetting->GetMotionFileName(group, no);

    // 例如 idle_0。
    csmString name = Utils::CubismString::GetFormatedString("%s_%d", group, no);
    CubismMotion* motion = static_cast<CubismMotion*>(_motions[name.GetRawString()]);
    csmBool autoDelete = false;

    if (motion == NULL)
    {
        csmString path = fileName;
        path = _modelHomeDir + path;

        csmByte* buffer;
        csmSizeInt size;
        buffer = CreateBuffer(path.GetRawString(), &size);
        motion = static_cast<CubismMotion*>(LoadMotion(buffer, size, NULL, onFinishedMotionHandler, onBeganMotionHandler, _modelSetting, group, no));

        if (motion)
        {
            motion->SetEffectIds(_eyeBlinkIds, _lipSyncIds);
            autoDelete = true; // 播放结束时释放内存。
        }

        DeleteBuffer(buffer, path.GetRawString());
    }
    else
    {
        motion->SetBeganMotionHandler(onBeganMotionHandler);
        motion->SetFinishedMotionHandler(onFinishedMotionHandler);
    }

    // 语音。
    csmString voice = _modelSetting->GetMotionSoundFileName(group, no);
    if (strcmp(voice.GetRawString(), "") != 0)
    {
        csmString path = voice;
        path = _modelHomeDir + path;
        _wavFileHandler.Start(path);
    }

    if (_debugMode)
    {
        LAppPal::PrintLogLn("[APP]start motion: [%s_%d]", group, no);
    }
    return  _motionManager->StartMotionPriority(motion, autoDelete, priority);
}

CubismMotionQueueEntryHandle LAppModel::StartRandomMotion(const csmChar* group, csmInt32 priority, ACubismMotion::FinishedMotionCallback onFinishedMotionHandler, ACubismMotion::BeganMotionCallback onBeganMotionHandler)
{
    if (_modelSetting->GetMotionCount(group) == 0)
    {
        if (_autoMotions.GetSize() > 0)
        {
            csmInt32 no = rand() % _autoMotions.GetSize();
            ACubismMotion* motion = _autoMotions[no];
            motion->SetFinishedMotionHandler(onFinishedMotionHandler);
            motion->SetBeganMotionHandler(onBeganMotionHandler);
            return _motionManager->StartMotionPriority(motion, false, priority);
        }
        return InvalidMotionQueueEntryHandleValue;
    }

    csmInt32 no = rand() % _modelSetting->GetMotionCount(group);

    return StartMotion(group, no, priority, onFinishedMotionHandler, onBeganMotionHandler);
}

void LAppModel::DoDraw()
{
    if (_model == NULL)
    {
        return;
    }

    GetRenderer<Rendering::CubismRenderer_OpenGLES2>()->DrawModel();
}

void LAppModel::Draw(CubismMatrix44& matrix)
{
    if (_model == NULL)
    {
        return;
    }

    matrix.MultiplyByMatrix(_modelMatrix);

    GetRenderer<Rendering::CubismRenderer_OpenGLES2>()->SetMvpMatrix(&matrix);

    DoDraw();
}

csmBool LAppModel::HitTest(const csmChar* hitAreaName, csmFloat32 x, csmFloat32 y)
{
    // 透明时不参与命中测试。
    if (_opacity < 1)
    {
        return false;
    }
    const csmInt32 count = _modelSetting->GetHitAreasCount();
    for (csmInt32 i = 0; i < count; i++)
    {
        if (strcmp(_modelSetting->GetHitAreaName(i), hitAreaName) == 0)
        {
            const CubismIdHandle drawID = _modelSetting->GetHitAreaId(i);
            return IsHit(drawID, x, y);
        }
    }
    
    if (count == 0) {
        // 为未配置 HitAreas 的模型（如薇薇安）提供回退判定。
        // 检查通用的上下半区。
        if (strcmp(hitAreaName, HitAreaNameHead) == 0) {
            return y > 0.0f; // 逻辑空间的上半区。
        } else if (strcmp(hitAreaName, HitAreaNameBody) == 0) {
            return y <= 0.0f; // 下半区。
        }
    }
    
    return false; // 区域不存在时返回 false。
}

void LAppModel::SetExpression(const csmChar* expressionID)
{
    ACubismMotion* motion = _expressions[expressionID];
    if (_debugMode)
    {
        LAppPal::PrintLogLn("[APP]expression: [%s]", expressionID);
    }

    if (motion != NULL)
    {
        _expressionManager->StartMotion(motion, false);
        _lastExpressionTime = _userTimeSeconds;
    }
    else
    {
        if (_debugMode) LAppPal::PrintLogLn("[APP]expression[%s] is null ", expressionID);
    }
}

void LAppModel::SetRandomExpression()
{
    if (_expressions.GetSize() == 0)
    {
        return;
    }

    csmInt32 no = _nextExpressionIndex % _expressions.GetSize();
    _nextExpressionIndex = (no + 1) % _expressions.GetSize();
    csmMap<csmString, ACubismMotion*>::const_iterator map_ite;
    csmInt32 i = 0;
    for (map_ite = _expressions.Begin(); map_ite != _expressions.End(); map_ite++)
    {
        if (i == no)
        {
            csmString name = (*map_ite).First;
            SetExpression(name.GetRawString());
            return;
        }
        i++;
    }
}

void LAppModel::ReloadRenderer()
{
    DeleteRenderer();

    CreateRenderer(_viewportWidth, _viewportHeight);

    SetupTextures();
}

void LAppModel::SwitchSkin()
{
    csmInt32 groupCount = _modelSetting->GetMotionGroupCount();
    if (groupCount == 0) {
        if (_expressions.GetSize() > 0) {
            LAppPal::PrintLogLn("[APP] No motion groups, cycling expression instead");
            SetRandomExpression();
        } else {
            LAppPal::PrintLogLn("[APP] No motion groups or expressions available for skin switching");
        }
        return;
    }

    // 切换前，将全部皮肤参数恢复为默认值。
    for (csmInt32 i = 0; i < _allSkinParamIds.GetSize(); i++)
    {
        _model->SetParameterValue(_allSkinParamIds[i], _allSkinParamDefaults[i]);
    }
    _model->SaveParameters();

    _currentSkinIndex = (_currentSkinIndex + 1) % groupCount;
    const csmChar* group = _modelSetting->GetMotionGroupName(_currentSkinIndex);
    LAppPal::PrintLogLn("[APP] Switching skin to group: %s (index %d)", group, _currentSkinIndex);
    StartMotion(group, 0, PriorityForce);
}

bool LAppModel::SetupTextures()
{
    for (csmInt32 modelTextureNumber = 0; modelTextureNumber < _modelSetting->GetTextureCount(); modelTextureNumber++)
    {
        if (strcmp(_modelSetting->GetTextureFileName(modelTextureNumber), "") == 0) continue;

        csmString texturePath = _modelSetting->GetTextureFileName(modelTextureNumber);
        texturePath = _modelHomeDir + texturePath;

        GLuint textureId = 0;
        const std::string path = texturePath.GetRawString();
        for (const auto& lease : _textureLeases)
            if (lease.Key() == path) { textureId = lease.IdValue(); break; }
        if (!textureId) {
            auto lease = _textures.AcquireTexture(path);
            if (!lease) return false;
            textureId = lease.IdValue();
            _textureLeases.push_back(std::move(lease));
        }

        GetRenderer<Rendering::CubismRenderer_OpenGLES2>()->BindTexture(modelTextureNumber, textureId);
    }

    GetRenderer<Rendering::CubismRenderer_OpenGLES2>()->IsPremultipliedAlpha(false);
    return true;
}

std::size_t LAppModel::TextureBytes() const
{
    std::size_t sum = 0;
    for (const auto& lease : _textureLeases) sum += lease.Bytes();
    return sum;
}

void LAppModel::MotionEventFired(const csmString& eventValue)
{
    CubismLogInfo("%s is fired on LAppModel!!", eventValue.GetRawString());
}

Csm::Rendering::CubismRenderTarget_OpenGLES2& LAppModel::GetRenderBuffer()
{
    return _renderBuffer;
}

std::vector<std::string> LAppModel::GetExpressionIds() const
{
    std::vector<std::string> result;
    for (csmMap<csmString, ACubismMotion*>::const_iterator iter = _expressions.Begin();
         iter != _expressions.End(); ++iter)
    {
        result.push_back(std::string(iter->First.GetRawString()));
    }
    return result;
}

std::vector<LAppModel::MotionInfo> LAppModel::GetMotionList() const
{
    std::vector<MotionInfo> result;
    if (!_modelSetting) return result;

    for (csmInt32 g = 0; g < _modelSetting->GetMotionGroupCount(); g++)
    {
        const csmChar* group = _modelSetting->GetMotionGroupName(g);
        csmInt32 count = _modelSetting->GetMotionCount(group);
        for (csmInt32 i = 0; i < count; i++)
        {
            MotionInfo info;
            info.group = group;
            info.index = i;
            info.file = _modelSetting->GetMotionFileName(group, i);
            result.push_back(info);
        }
    }
    return result;
}
