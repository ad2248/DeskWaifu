#pragma once

#include "ApplicationService.hpp"
#include "ModelHotkeys.hpp"
#include "ModelRegion.hpp"
#include "ModelHitTest.hpp"
#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>
#include <QMetaType>
#include <memory>
#include <vector>

struct ModelRuntimeOptions
{
    std::string resourceBase, primaryDirectory, defaultModel;
    std::vector<std::string> additionalDirectories;
    float emotionTimeout = 5.0f;
};

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

struct ModelMetadata
{
    int index = -1;
    QStringList names;
    QJsonObject hotkeys;
    QJsonArray expressions, motions;
    std::vector<ModelHotkeys::Binding> bindings;
};

Q_DECLARE_METATYPE(ModelRenderState)
Q_DECLARE_METATYPE(ModelMetadata)
