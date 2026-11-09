#pragma once

#include "contracts/AppState.hpp"
#include "contracts/ModelFrameInput.hpp"
#include "contracts/ModelBinding.hpp"
#include "contracts/ModelRegion.hpp"
#include "ModelHitTest.hpp"
#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>
#include <QMetaType>
#include <memory>
#include <vector>

// 启动时复制给渲染线程的资源发现及模型初始化选项。
struct ModelRuntimeOptions
{
    std::string resourceBase, primaryDirectory, defaultModel;
    std::vector<std::string> additionalDirectories;
    float emotionTimeout = 5.0f;
};

// GUI 发布给渲染线程的最新输入快照；只包含可复制的值。
struct ModelRenderState
{
    AppSnapshot app;
    ModelFrameInput controls;
    int width = 1, height = 1;
    bool editing = false;
    std::string modelToken; // 现有后端的模型实例标识，不用于事件关联。
    bool hasLook = false;
    float lookX = 0, lookY = 0, opacity = 1;
    ModelRegion clip{0, 0, 1, 1};
    std::vector<ModelRegion> handles;
};

// 仅包含不可变 CPU 数据；GL 标识、Cubism 对象和窗口对象不跨线程传递。
struct RenderFrame
{
    int width = 0, height = 0;
    int modelIndex = -1;
    bool hidden = false;
    std::vector<unsigned char> rgba; // 自下而上、预乘 alpha 的 RGBA8 数据。
    std::vector<ModelRegion> regions; // 应用显示透明度之前的区域，单位为逻辑输入像素。
    std::shared_ptr<const ModelHitSnapshot> hit;
};

// 当前模型的目录、动作和绑定能力快照，可脱离 SDK 对象传递。
struct ModelMetadata
{
    int index = -1;
    QStringList names;
    QJsonObject hotkeys;
    QJsonArray expressions, motions;
    std::vector<ModelBinding> bindings;
};

Q_DECLARE_METATYPE(ModelRenderState)
Q_DECLARE_METATYPE(ModelMetadata)
