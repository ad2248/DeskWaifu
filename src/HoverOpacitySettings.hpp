#pragma once

#include "ApplicationService.hpp"
#include "LAppConfig.hpp"

// Hover settings are saved independently of the transform edit transaction.
// Publish runtime values only after a successful atomic configuration write.
class HoverOpacitySettings
{
public:
    explicit HoverOpacitySettings(LAppConfig& config)
        : _config(config), _store(config.sourcePath.empty() ? LAppConfig::GetDefaultConfigPath() : config.sourcePath) {}

    bool Enabled() const { return _config.values.hoverOpacityEnabled; }
    float Opacity() const { return _config.values.hoverOpacity; }

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
        ConfigStore::Snapshot request, latest, saved;
        std::string error;
        if (!ConfigStore::Parse(json, request, error)) return AppResult::Failure(error);
        const auto enabled = request.values.find("enabled");
        const auto opacity = request.values.find("opacity");
        if (enabled == request.values.end() && opacity == request.values.end())
            return AppResult::Failure("Specify enabled or opacity");
        if (!_store.Read(latest, error)) return AppResult::Failure(error);
        auto proposed = latest;
        if (enabled != request.values.end()) proposed.values["hover_opacity_enabled"] = enabled->second;
        if (opacity != request.values.end()) proposed.values["hover_opacity"] = opacity->second;
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
