#pragma once

#include "ApplicationService.hpp"
#include "ConfigStore.hpp"
#include <stdexcept>

// Hover settings are saved independently of the transform edit transaction.
// Publish runtime values only after a successful atomic configuration write.
class HoverOpacitySettings
{
public:
    HoverOpacitySettings(std::string path, bool enabled = false, float opacity = 0.5f)
        : _store(std::move(path)), _enabled(enabled), _opacity(opacity) {}

    bool Enabled() const { return _enabled; }
    float Opacity() const { return _opacity; }

    AppResult Refresh()
    {
        ConfigStore::Snapshot snapshot;
        std::string error;
        bool enabled;
        float opacity;
        if (!_store.Read(snapshot, error) || !Decode(snapshot, enabled, opacity, error))
            return AppResult::Failure(error);
        _enabled = enabled;
        _opacity = opacity;
        return AppResult::Success();
    }

    AppResult Set(const std::string& json)
    {
        ConfigStore::Snapshot request, latest, saved;
        std::string error;
        if (!ConfigStore::Parse(json, request, error)) return AppResult::Failure(error);
        ConfigStore::Patch patch;
        const auto enabled = request.values.find("enabled");
        const auto opacity = request.values.find("opacity");
        if (enabled != request.values.end()) patch["hover_opacity_enabled"] = enabled->second;
        if (opacity != request.values.end()) patch["hover_opacity"] = opacity->second;
        if (patch.empty()) return AppResult::Failure("Specify enabled or opacity");
        if (!_store.Read(latest, error)) return AppResult::Failure(error);
        auto proposed = latest;
        for (const auto& entry : patch) proposed.values[entry.first] = entry.second;
        bool nextEnabled;
        float nextOpacity;
        if (!Decode(proposed, nextEnabled, nextOpacity, error)) return AppResult::Failure(error);
        if (!_store.Commit(patch, latest, saved, error)) return AppResult::Failure(error);
        _enabled = nextEnabled;
        _opacity = nextOpacity;
        return AppResult::Success();
    }

private:
    static bool Decode(const ConfigStore::Snapshot& snapshot, bool& enabled, float& opacity, std::string& error)
    {
        enabled = false;
        opacity = 0.5f;
        const auto flag = snapshot.values.find("hover_opacity_enabled");
        if (flag != snapshot.values.end()) {
            if (flag->second != "true" && flag->second != "false") {
                error = "hover_opacity_enabled must be boolean";
                return false;
            }
            enabled = flag->second == "true";
        }
        const auto value = snapshot.values.find("hover_opacity");
        if (value != snapshot.values.end()) {
            try {
                size_t end;
                const double number = std::stod(value->second, &end);
                if (end != value->second.size() || !std::isfinite(number) || number < 0 || number > 1)
                    throw std::out_of_range("opacity");
                opacity = static_cast<float>(number);
            } catch (...) {
                error = "hover_opacity must be a finite number within 0..1";
                return false;
            }
        }
        return true;
    }

    ConfigStore _store;
    bool _enabled;
    float _opacity;
};
