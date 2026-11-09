#pragma once

#include <string>
#include <experimental/optional>
#include <vector>
#include "contracts/ModelRegion.hpp"
#include "contracts/ConfigStore.hpp"

// 桌面插件的持久化配置及其来源路径；不持有运行中的窗口或模型。
// 启动单例供装配阶段读取，交互域可持有副本；修改须由调用方串行化。
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
        bool fullscreenOpacityEnabled = false;
        float fullscreenOpacity = 0.5f;
        float mouseLookRadius = 0.5f;
        std::experimental::optional<ModelRegion> modelRegion;
        // 宿主不解释的字段在保存其他设置时原样保留。
        std::map<std::string, std::string> unknownFields;
    };
    Values values;
    std::string sourcePath;

    static LAppConfig& GetInstance();

    // 记录来源路径并加载 JSON；成功才替换 values。无法打开文件时返回 false，
    // diagnostic 为空；解析或校验失败时返回 false 并写入诊断。
    bool LoadFromFile(const std::string& path, std::string* diagnostic = nullptr);
    // 以默认值补全缺失字段并保留未知字段；失败写入 error，不改写 values。
    static bool Decode(const ConfigStore::Snapshot& snapshot, Values& values, std::string& error);
    // 生成并重新校验 JSON；成功才写入 document，失败通过 error 返回原因。
    static bool Serialize(const Values& values, std::string& document, std::string& error);

    // 优先使用 XDG_CONFIG_HOME，否则返回用户目录下的默认配置路径。
    static std::string GetDefaultConfigPath();

    // 将开头的波浪号替换为当前用户主目录，不执行 shell 展开或用户名查找。
    static std::string ExpandTilde(const std::string& path);
};
