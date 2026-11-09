#pragma once

#include "contracts/AppState.hpp"
#include "LAppConfig.hpp"

// 配置保存成功后才更新鼠标视线范围限制。
class MouseLookSettings
{
public:
    explicit MouseLookSettings(LAppConfig& config)
        : _config(config), _store(config.sourcePath.empty() ? LAppConfig::GetDefaultConfigPath() : config.sourcePath) {}

    float Radius() const { return _config.values.mouseLookRadius; }

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

    AppResult Set(const ConfigStore::Snapshot& request)
    {
        ConfigStore::Snapshot latest, saved;
        std::string error;
        const auto radius = request.values.find("radius");
        if (radius == request.values.end()) return AppResult::Failure("Specify radius");
        if (!_store.Read(latest, error)) return AppResult::Failure(error);
        auto proposed = latest;
        proposed.values["mouse_look_radius"] = radius->second;
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
