#pragma once

#include "ApplicationService.hpp"
#include "LAppConfig.hpp"

// Credentials are write-only over IPC; only presence is queried by the GUI.
class SpeechSettings
{
public:
    explicit SpeechSettings(LAppConfig& config) : _config(config),
        _store(config.sourcePath.empty() ? LAppConfig::GetDefaultConfigPath() : config.sourcePath) {}

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

    AppResult Set(const ConfigStore::Snapshot& request)
    {
        const auto key = request.values.find("api_key");
        std::string value;
        if (key == request.values.end() || !ConfigStore::ParseString(key->second, value) ||
            value.size() > 4096 || value.find_first_of("\r\n") != std::string::npos)
            return AppResult::Failure("Invalid MiniMax API Key");
        ConfigStore::Snapshot latest, saved;
        std::string error;
        if (!_store.Read(latest, error)) return AppResult::Failure(error);
        auto proposed = latest;
        proposed.values["minimax_api_key"] = key->second;
        LAppConfig::Values values;
        std::string document;
        if (!LAppConfig::Decode(proposed, values, error) || !LAppConfig::Serialize(values, document, error) ||
            !_store.Commit(document, latest, saved, error)) return AppResult::Failure(error);
        _config.values = std::move(values);
        return AppResult::Success();
    }

    bool Configured() const { return !_config.values.minimaxApiKey.empty(); }
private:
    LAppConfig& _config;
    ConfigStore _store;
};
