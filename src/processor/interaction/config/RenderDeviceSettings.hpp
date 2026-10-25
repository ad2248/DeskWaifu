#pragma once

#include "contracts/AppState.hpp"
#include "LAppConfig.hpp"

// 此设置用于下次启动，不代表当前已绑定的渲染上下文。
class RenderDeviceSettings
{
public:
    explicit RenderDeviceSettings(LAppConfig& config)
        : _config(config), _store(config.sourcePath.empty() ? LAppConfig::GetDefaultConfigPath() : config.sourcePath) {}

    std::string Device() const { return _config.values.renderDevice.empty() ? "auto" : _config.values.renderDevice; }

    AppResult Refresh()
    {
        ConfigStore::Snapshot snapshot;
        LAppConfig::Values values;
        std::string error;
        if (!_store.Read(snapshot, error) || !LAppConfig::Decode(snapshot, values, error)) return AppResult::Failure(error);
        _config.values = std::move(values);
        return AppResult::Success();
    }

    AppResult Set(const ConfigStore::Snapshot& request)
    {
        const auto device = request.values.find("device");
        if (device == request.values.end()) return AppResult::Failure("Specify device");
        ConfigStore::Snapshot latest, saved;
        std::string error, document;
        if (!_store.Read(latest, error)) return AppResult::Failure(error);
        auto proposed = latest;
        proposed.values["render_device"] = device->second;
        LAppConfig::Values values;
        if (!LAppConfig::Decode(proposed, values, error) || !LAppConfig::Serialize(values, document, error) ||
            !_store.Commit(document, latest, saved, error)) return AppResult::Failure(error);
        _config.values = std::move(values);
        return AppResult::Success();
    }

private:
    LAppConfig& _config;
    ConfigStore _store;
};
