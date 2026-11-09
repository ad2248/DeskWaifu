#pragma once

#include "contracts/AppState.hpp"
#include "LAppConfig.hpp"

// 悬停配置独立于变换编辑事务保存。
// 仅在配置原子写入成功后更新运行时数值。
class HoverOpacitySettings
{
public:
    explicit HoverOpacitySettings(LAppConfig& config)
        : _config(config), _store(config.sourcePath.empty() ? LAppConfig::GetDefaultConfigPath() : config.sourcePath) {}

    bool Enabled() const { return _config.values.hoverOpacityEnabled; }
    float Opacity() const { return _config.values.hoverOpacity; }
    float IdleOpacity() const { return _config.values.idleOpacity; }
    bool FullscreenEnabled() const { return _config.values.fullscreenOpacityEnabled; }
    float FullscreenOpacity() const { return _config.values.fullscreenOpacity; }

    // 从磁盘读取并校验完整配置；失败返回错误且不更新内存配置。
    AppResult Refresh()
    {
        ConfigStore::Snapshot snapshot;
        LAppConfig::Values values;
        std::string error;
        if (!_store.Read(snapshot, error) || !LAppConfig::Decode(snapshot, values, error))
            return AppResult::Failure(error);
        _config.values = std::move(values);
        return AppResult::Success();
    }

    AppResult Set(const std::string& json)
    {
        ConfigStore::Snapshot request;
        std::string error;
        if (!ConfigStore::Parse(json, request, error)) return AppResult::Failure(error);
        return Set(request);
    }

    // 将请求中的透明度字段合并到最新配置；至少需要一个受支持字段。
    // 校验、并发冲突或写入失败通过返回值报告；借用 config 并串行更新。
    AppResult Set(const ConfigStore::Snapshot& request)
    {
        ConfigStore::Snapshot latest, saved;
        std::string error;
        const auto enabled = request.values.find("enabled");
        const auto opacity = request.values.find("opacity");
        const auto idleOpacity = request.values.find("idle_opacity");
        const auto fullscreenEnabled = request.values.find("fullscreen_enabled");
        const auto fullscreenOpacity = request.values.find("fullscreen_opacity");
        if (enabled == request.values.end() && opacity == request.values.end() && idleOpacity == request.values.end()
            && fullscreenEnabled == request.values.end() && fullscreenOpacity == request.values.end())
            return AppResult::Failure("Specify enabled, opacity or idle_opacity");
        if (!_store.Read(latest, error)) return AppResult::Failure(error);
        auto proposed = latest;
        if (enabled != request.values.end()) proposed.values["hover_opacity_enabled"] = enabled->second;
        if (opacity != request.values.end()) proposed.values["hover_opacity"] = opacity->second;
        if (idleOpacity != request.values.end()) proposed.values["idle_opacity"] = idleOpacity->second;
        if (fullscreenEnabled != request.values.end()) proposed.values["fullscreen_opacity_enabled"] = fullscreenEnabled->second;
        if (fullscreenOpacity != request.values.end()) proposed.values["fullscreen_opacity"] = fullscreenOpacity->second;
        LAppConfig::Values values;
        std::string document;
        if (!LAppConfig::Decode(proposed, values, error) || !LAppConfig::Serialize(values, document, error) ||
            !_store.Commit(document, latest, saved, error)) return AppResult::Failure(error);
        _config.values = std::move(values);
        return AppResult::Success();
    }

private:
    LAppConfig& _config;
    ConfigStore _store;
};
