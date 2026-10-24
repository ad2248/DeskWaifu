#pragma once

#include "ApplicationService.hpp"
#include "ModelHotkeys.hpp"
#include "ModelRegion.hpp"
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
    std::string modelToken; // Existing backend model incarnation, not event correlation.
    bool hasLook = false;
    float lookX = 0, lookY = 0, opacity = 1;
    ModelRegion clip{0, 0, 1, 1};
    std::vector<ModelRegion> handles;
};

// Immutable CPU data only. GL names, Cubism objects and window objects never cross threads.
struct RenderFrame
{
    int width = 0, height = 0;
    int modelIndex = -1;
    bool hidden = false;
    std::vector<unsigned char> rgba; // Bottom-up, premultiplied RGBA8.
    std::vector<ModelRegion> regions; // Before display opacity, in logical input pixels.
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
