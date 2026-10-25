/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */
#pragma once

#include <CubismFramework.hpp>
#include <Math/CubismMatrix44.hpp>
#include <Type/csmVector.hpp>
#include <functional>
#include <string>
#include <vector>
#include "ModelTransform.hpp"
#include "ModelFrameInput.hpp"
#include "ModelCache.hpp"
#include "ModelSession.hpp"
#include "ModelLoader.hpp"

class LAppModel;
class LAppTextureManager;

struct ModelDiscoveryOptions
{
    std::string primaryDirectory;
    std::vector<std::string> additionalDirectories;
    std::string defaultModel;
    float emotionTimeout = 5.0f;
};

/**
* @brief 在应用中管理 CubismModel 的类。<br>
*        负责模型创建和销毁、点击事件处理以及模型切换。
*
*/
class LAppLive2DManager
{

public:
    /**
    * @brief 返回类的单例。<br>
    *        实例尚未创建时，在内部创建。
    *
    * @return 类实例。
    */

    /**
    * @brief 释放类的单例。
    *
    */

    /**
    * @brief 获取 Resources 下的模型目录名称。
    *
    */
    Csm::csmVector<Csm::csmString> GetModelDir() const;

    /**
    * @brief 获取 Resources 下的模型目录数量。
    *
    */
    Csm::csmInt32 GetModelDirSize() const;

    /**
    * @brief 返回当前场景持有的模型。
    *
    * @param[in]   no 模型列表中的索引。
    * @return 模型实例；索引越界时返回 NULL。
    */
    LAppModel* GetModel(Csm::csmUint32 no) const;

    /**
     * @brief 设置模型离屏渲染尺寸。
     *
     * @param[in]   width 窗口宽度。
     * @param[in]   height 窗口高度。
     */
    void SetRenderTargetSize(Csm::csmUint32 width, Csm::csmUint32 height);

    /**
    * @brief 释放当前场景持有的全部模型。
    *
    */
    void ReleaseAllModel();

    /**
    * @brief 处理画面拖动。
    *
    * @param[in]   x 画面 X 坐标。
    * @param[in]   y 画面 Y 坐标。
    */
    void OnDrag(Csm::csmFloat32 x, Csm::csmFloat32 y) const;

    /**
    * @brief 处理画面点击。
    *
    * @param[in]   x 裁剪空间 X 坐标（-1..1）。
    * @param[in]   y 裁剪空间 Y 坐标（-1..1）。
    */
    void OnTap(Csm::csmFloat32 x, Csm::csmFloat32 y, int width, int height, ModelTransform transform);

    /**
    * @brief 处理画面更新。
    *        更新并绘制模型。
    */
    void OnUpdate(int width, int height, ModelTransform transform, const ModelFrameInput& input,
        const std::function<void(LAppModel&)>& beforeDraw,
        const std::function<void(LAppModel&)>& afterDraw) const;

    /**
    * @brief 切换到下一个场景。<br>
    *        示例应用中用于切换模型集合。
    */
    bool NextScene();

    /**
    * @brief 切换当前模型的皮肤。
    */
    void SwitchSkin();

    /**
    * @brief 切换场景。<br>
    *        示例应用中用于切换模型集合。
    */
    bool ChangeScene(Csm::csmInt32 index);

    /**
     * @brief 获取模型数量。
     * @return 持有的模型数量。
     */
    Csm::csmUint32 GetModelNum() const;

    /**
     * @brief 设置 viewMatrix。
     */
    void SetViewMatrix(Live2D::Cubism::Framework::CubismMatrix44* m);

    /**
     * @brief 获取当前场景（模型）的索引。
     */
    Csm::csmInt32 GetSceneIndex() const { return _session.CurrentIndex(); }

public:
    LAppLive2DManager(LAppTextureManager& textures, int width, int height,
        ModelDiscoveryOptions discovery);
    virtual ~LAppLive2DManager();
    bool HasModels() const { return _session.Current() != nullptr; }

private:
    void SetUpModel();
    /**
    * @brief 构造函数。
    */

    /**
    * @brief 析构函数。
    */

    Csm::CubismMatrix44* _viewMatrix; ///< 模型绘制使用的视图矩阵。
    LAppTextureManager& _textures;
    int _viewportWidth;
    int _viewportHeight;
    ModelDiscoveryOptions _discovery;
    ModelLoader _loader;
    std::vector<ModelDescriptor> _descriptors;
    Csm::csmVector<LAppModel*> _models; ///< 当前显示模型的容器，所有权属于缓存。
    ModelSession<LAppModel> _session; ///< 活动模型、缓存和驻留策略。
    std::unique_ptr<LAppModel> _sampleModel;

    Csm::csmVector<Csm::csmString> _modelDir; ///< 模型目录名称容器。
};
