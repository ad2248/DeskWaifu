#pragma once

#include <string>
#include <experimental/optional>
#include <vector>
#include "ModelRegion.hpp"
#include "ConfigStore.hpp"

struct LAppConfig
{
    // Only persisted fields belong here; runtime bookkeeping stays outside.
    struct Values
    {
        std::vector<std::string> additionalModelDirs;
        std::string defaultModel;
        std::string modelOutput;
        std::string minimaxApiKey;
        float emotionTimeout = 5.0f;
        float modelScale = 1.0f;
        float modelX = 0.0f;
        float modelY = 0.0f;
        int windowWidth = 1900;
        int windowHeight = 1000;
        bool hoverOpacityEnabled = false;
        float hoverOpacity = 0.5f;
        float idleOpacity = 1.0f;
        float mouseLookRadius = 0.5f;
        std::experimental::optional<ModelRegion> modelRegion;
    };
    Values values;
    std::string sourcePath;

    static LAppConfig& GetInstance();

    bool LoadFromFile(const std::string& path);
    static bool Decode(const ConfigStore::Snapshot& snapshot, Values& values, std::string& error);
    static bool Serialize(const Values& values, std::string& document, std::string& error);

    static std::string GetDefaultConfigPath();

    static std::string ExpandTilde(const std::string& path);
};
