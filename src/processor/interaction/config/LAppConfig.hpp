#pragma once

#include <string>
#include <experimental/optional>
#include <vector>
#include "contracts/ModelRegion.hpp"
#include "contracts/ConfigStore.hpp"

struct LAppConfig
{
    static std::string GetDefaultPluginDir();

    // 此处只放持久化字段，运行时管理状态保存在外部。
    struct Values
    {
        std::vector<std::string> additionalModelDirs;
        std::string defaultModel;
        std::string modelOutput;
        std::string renderDevice;
        std::string pluginDir = GetDefaultPluginDir();
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
        // 宿主不解释的字段在保存其他设置时原样保留。
        std::map<std::string, std::string> unknownFields;
    };
    Values values;
    std::string sourcePath;

    static LAppConfig& GetInstance();

    bool LoadFromFile(const std::string& path, std::string* diagnostic = nullptr);
    static bool Decode(const ConfigStore::Snapshot& snapshot, Values& values, std::string& error);
    static bool Serialize(const Values& values, std::string& document, std::string& error);

    static std::string GetDefaultConfigPath();

    static std::string ExpandTilde(const std::string& path);
};
