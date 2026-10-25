/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#pragma once

#include <string>
#include <vector>
#include <CubismFramework.hpp>
#include <ICubismModelSetting.hpp>
#include <Type/csmRectF.hpp>
#include <Rendering/OpenGL/CubismRenderTarget_OpenGLES2.hpp>

#include "LAppWavFileHandler_Common.hpp"
#include "LAppModel_Common.hpp"
#include "ModelHotkeys.hpp"
#include "ModelFrameInput.hpp"
#include "LAppTextureManager.hpp"

/**
 * @brief 用户实际使用的模型实现类。<br>
 *        负责模型和功能组件的创建、更新及渲染调用。
 *
 */
class LAppModel : public LAppModel_Common
{
public:
    /**
     * @brief 循环切换动作组，以切换到下一套皮肤。
     */
    void SwitchSkin();
    ModelHotkeys& Hotkeys() { return _hotkeys; }

    /**
     * @brief 构造函数。
     */
    LAppModel(class LAppTextureManager& textures, int width, int height, float emotionTimeout);
    void SetViewport(int width, int height) { _viewportWidth = width; _viewportHeight = height; }

    /**
     * @brief 析构函数。
     *
     */
    virtual ~LAppModel();

    /**
     * @brief 根据 model3.json 所在目录及文件路径创建模型。
     *
     */
    bool LoadAssets(const Csm::csmChar* dir, const Csm::csmChar* fileName);

    /**
     * @brief 重建渲染器。
     *
     */
    void ReloadRenderer();

    /**
     * @brief 更新模型，根据模型参数确定绘制状态。
     *
     */
    void Update(const ModelFrameInput& input);

    /**
     * @brief 绘制模型，传入模型绘制空间的视图投影矩阵。
     *
     * @param[in]  matrix 视图投影矩阵。
     */
    void Draw(Csm::CubismMatrix44& matrix);

    /**
     * @brief 开始播放参数指定的动作。
     *
     * @param[in]   group 动作组名称。
     * @param[in]   no 组内编号。
     * @param[in]   priority 优先级。
     * @param[in]   onFinishedMotionHandler 动作播放结束时的回调；为 NULL 时不调用。
     * @param[in]   onBeganMotionHandler 动作播放开始时的回调；为 NULL 时不调用。
     * @return 已开始动作的标识，用作 IsFinished() 的参数判断动作是否结束；无法开始时返回 -1。
     */
    Csm::CubismMotionQueueEntryHandle StartMotion(const Csm::csmChar* group, Csm::csmInt32 no, Csm::csmInt32 priority, Csm::ACubismMotion::FinishedMotionCallback onFinishedMotionHandler = NULL, Csm::ACubismMotion::BeganMotionCallback onBeganMotionHandler = NULL);

    /**
     * @brief 开始播放随机选中的动作。
     *
     * @param[in]   group 动作组名称。
     * @param[in]   priority 优先级。
     * @param[in]   onFinishedMotionHandler 动作播放结束时的回调；为 NULL 时不调用。
     * @param[in]   onBeganMotionHandler 动作播放开始时的回调；为 NULL 时不调用。
     * @return 已开始动作的标识，用作 IsFinished() 的参数判断动作是否结束；无法开始时返回 -1。
     */
    Csm::CubismMotionQueueEntryHandle StartRandomMotion(const Csm::csmChar* group, Csm::csmInt32 priority, Csm::ACubismMotion::FinishedMotionCallback onFinishedMotionHandler = NULL, Csm::ACubismMotion::BeganMotionCallback onBeganMotionHandler = NULL);

    /**
     * @brief 设置参数指定的表情动作。
     *
     * @param   expressionID 表情动作 ID。
     */
    void SetExpression(const Csm::csmChar* expressionID);

    /**
     * @brief 设置随机选中的表情动作。
     *
     */
    void SetRandomExpression();

    /**
    * @brief 接收事件触发通知。
    *
    */
    virtual void MotionEventFired(const Live2D::Cubism::Framework::csmString& eventValue);

    /**
     * @brief 命中测试。<br>
     *        根据指定 ID 的顶点列表计算矩形，判断坐标是否位于矩形内。
     *
     * @param[in]   hitAreaName 命中测试目标的 ID。
     * @param[in]   x 待测试的 X 坐标。
     * @param[in]   y 待测试的 Y 坐标。
     */
    virtual Csm::csmBool HitTest(const Csm::csmChar* hitAreaName, Csm::csmFloat32 x, Csm::csmFloat32 y);

    /**
     * @brief 获取绘制到其他目标时使用的缓冲区。
     */
    Csm::Rendering::CubismRenderTarget_OpenGLES2& GetRenderBuffer();

    /**
     * @brief 获取全部表情 ID，供 IPC 查询。
     */
    std::vector<std::string> GetExpressionIds() const;

    /**
     * @brief 供 IPC 查询的动作信息。
     */
    struct MotionInfo {
        std::string group;
        int index;
        std::string file;
    };

    /**
     * @brief 获取全部可用动作，供 IPC 查询。
     */
    std::vector<MotionInfo> GetMotionList() const;
    std::size_t TextureBytes() const;

    /**
     * @brief 获取模型配置，供 IPC 查询模型能力。
     */
    Csm::ICubismModelSetting* GetModelSetting() const { return _modelSetting; }

protected:
    /**
     * @brief 绘制模型，传入模型绘制空间的视图投影矩阵。
     *
     */
    void DoDraw();

private:
    LAppTextureManager& _textures;
    std::vector<LAppTextureManager::TextureLease> _textureLeases;
    int _viewportWidth;
    int _viewportHeight;
    float _emotionTimeout;
    ModelHotkeys _hotkeys;
    /**
     * @brief 根据 model3.json 创建模型。<br>
     *        按文件描述创建模型、动作和物理计算等组件。
     *
     * @param[in]   setting ICubismModelSetting 实例。
     *
     */
    void SetupModel(Csm::ICubismModelSetting* setting);

    /**
     * @brief 向 OpenGL 纹理单元加载纹理。
     *
     */
    bool SetupTextures();
    Csm::csmVector<Csm::ACubismMotion*> _autoMotions;

    /**
     * @brief 根据组名批量加载动作数据。<br>
     *        动作数据名称在内部从 ModelSetting 获取。
     *
     * @param[in]   group 动作数据组名。
     */
    void PreloadMotionGroup(const Csm::csmChar* group);

    /**
     * @brief 根据组名批量释放动作数据。<br>
     *        动作数据名称在内部从 ModelSetting 获取。
     *
     * @param[in]   group 动作数据组名。
     */
    void ReleaseMotionGroup(const Csm::csmChar* group) const;

    /**
    * @brief 释放全部动作数据。
    *
    * 释放全部动作数据。
    */
    void ReleaseMotions();

    /**
    * @brief 释放全部表情数据。
    *
    * 释放全部表情数据。
    */
    void ReleaseExpressions();

    Csm::ICubismModelSetting* _modelSetting; ///< 模型配置信息。
    Csm::csmString _modelHomeDir; ///< 模型配置所在目录。
    Csm::csmFloat32 _userTimeSeconds; ///< 累计帧间隔时间，单位为秒。
    Csm::csmVector<Csm::CubismIdHandle> _eyeBlinkIds; ///< 模型眨眼功能使用的参数 ID。
    Csm::csmVector<Csm::CubismIdHandle> _lipSyncIds; ///< 模型唇形同步功能使用的参数 ID。
    Csm::csmMap<Csm::csmString, Csm::ACubismMotion*>   _motions; ///< 已加载的动作列表。
    Csm::csmMap<Csm::csmString, Csm::ACubismMotion*>   _expressions; ///< 已加载的表情列表。
    Csm::csmVector<Csm::csmRectF> _hitArea;
    Csm::csmVector<Csm::csmRectF> _userArea;
    const Csm::CubismId* _idParamAngleX; ///< 参数 ID：ParamAngleX。
    const Csm::CubismId* _idParamAngleY; ///< 参数 ID：ParamAngleX。
    const Csm::CubismId* _idParamAngleZ; ///< 参数 ID：ParamAngleX。
    const Csm::CubismId* _idParamBodyAngleX; ///< 参数 ID：ParamBodyAngleX。
    
    const Csm::CubismId* _idParamBodyAngleY; ///< 参数 ID：ParamBodyAngleY。
    
    const Csm::CubismId* _idParamBodyAngleZ; ///< 参数 ID：ParamBodyAngleZ。
    const Csm::CubismId* _idParamEyeBallX; ///< 参数 ID：ParamEyeBallX。
    const Csm::CubismId* _idParamEyeBallY; ///< 参数 ID：ParamEyeBallXY。

    Csm::csmBool _motionUpdated; ///< 动作更新标志。
    Csm::csmInt32 _currentSkinIndex;

    Csm::csmFloat32 _lastExpressionTime; ///< 上次设置表情的时间。
    static const Csm::csmFloat32 ExpressionTimeoutSeconds; ///< 表情恢复默认值前的等待秒数。
    Csm::csmInt32 _nextExpressionIndex; ///< 循环切换表情的轮转索引。

    /// 各皮肤动作组中的全部参数 ID，用于切换前重置。
    Csm::csmVector<const Csm::CubismId*> _allSkinParamIds;
    /// 各皮肤参数的模型默认值。
    Csm::csmVector<Csm::csmFloat32> _allSkinParamDefaults;
    void CollectSkinParams(); ///< 扫描动作组 JSON，收集皮肤参数 ID。

    LAppWavFileHandler_Common _wavFileHandler; ///< WAV 文件处理器。

    Csm::Rendering::CubismRenderTarget_OpenGLES2 _renderBuffer;   ///< 默认帧缓冲以外的绘制目标。
};
