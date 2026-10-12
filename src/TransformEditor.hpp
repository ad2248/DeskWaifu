#pragma once

#include "ApplicationService.hpp"
#include "ConfigStore.hpp"
#include "InputController.hpp"
#include "ModelRegion.hpp"
#include <iomanip>
#include <limits>
#include <sstream>

struct TransformSettings
{
    ModelTransform transform{1, 0, 0};
    bool cropped = false;
    ModelRegion region;
};

// Owns the editing transaction, not the rendering/platform policy.
class TransformEditor
{
public:
    TransformEditor(ApplicationService& service, std::string path)
        : _service(service), _store(std::move(path)) {}

    bool Initialize(const TransformSettings& initial)
    {
        _runtime = _disk = initial;
        if (!_service.SetTransform(initial.transform).ok) return false;
        ConfigStore::Snapshot snapshot;
        if (!_store.Read(snapshot, _error)) { _loadFailed = true; return true; } // Never overwrite an invalid file.
        TransformSettings parsed;
        if (!Decode(snapshot, parsed, _error)) { _loadFailed = true; return true; }
        _snapshot = snapshot; _runtime = _disk = parsed;
        _service.SetTransform(parsed.transform);
        return true;
    }

    static bool Validate(const TransformSettings& value, std::string& error)
    {
        const auto t = value.transform;
        if (!std::isfinite(t.x) || !std::isfinite(t.y) || !std::isfinite(t.scale) || t.scale < .1f || t.scale > 10.f) {
            error = "Transform must be finite; zoom must be 0.1..10"; return false;
        }
        if (value.cropped && (value.region.width <= 0 || value.region.height <= 0)) {
            error = "Crop dimensions must be positive integers"; return false;
        }
        return true;
    }

    static bool Decode(const ConfigStore::Snapshot& snapshot, TransformSettings& value, std::string& error)
    {
        value = {};
        auto number = [&](const char* key, float& destination) {
            auto it = snapshot.values.find(key); if (it == snapshot.values.end()) return true;
            try { size_t end; destination = std::stof(it->second, &end); return end == it->second.size() && std::isfinite(destination); }
            catch (...) { return false; }
        };
        if (!number("model_x", value.transform.x) || !number("model_y", value.transform.y) || !number("model_scale", value.transform.scale)) {
            error = "Invalid model transform in config"; return false;
        }
        const auto region = snapshot.values.find("model_region");
        if (region != snapshot.values.end()) {
            ConfigStore::Snapshot rectangle;
            if (!ConfigStore::Parse(region->second, rectangle, error)) return false;
            auto integer = [&](const char* key, int& destination, bool required) {
                auto it = rectangle.values.find(key); if (it == rectangle.values.end()) return !required;
                try { size_t end; long long n = std::stoll(it->second, &end); if (end != it->second.size() || n < INT32_MIN || n > INT32_MAX) return false; destination = static_cast<int>(n); return true; }
                catch (...) { return false; }
            };
            if (!integer("x", value.region.x, false) || !integer("y", value.region.y, false) ||
                !integer("width", value.region.width, true) || !integer("height", value.region.height, true)) {
                error = "Invalid integer model_region"; return false;
            }
            value.cropped = true;
        }
        return Validate(value, error);
    }

    const TransformSettings& Display() const { return Editing() ? _draft : _runtime; }
    bool Editing() const { return !_owner.empty(); }
    bool OwnedBy(const std::string& owner) const { return Editing() && _owner == owner; }
    bool ScreenEditing() const { return Editing() && _screen; }
    bool Dirty() const { return _dirty; }
    bool Conflict() const { return _conflict; }
    const std::string& Error() const { return _error; }
    const std::string& Path() const { return _store.Path(); }
    const std::string& Owner() const { return _owner; }
    unsigned Revision() const { return _revision; }

    AppResult Begin(const std::string& owner, bool screen, int width, int height,
        unsigned expectedRevision = std::numeric_limits<unsigned>::max())
    {
        if (owner.empty()) return AppResult::Failure("Editing requires a connection");
        if (Editing() && !OwnedBy(owner)) return AppResult::Failure("Another client is editing transforms");
        if (!Editing()) {
            Reload();
            if (expectedRevision != std::numeric_limits<unsigned>::max() && expectedRevision != _revision)
                return AppResult::Failure("Settings changed; refresh before editing");
            SyncRuntime(); _draft = _runtime; _expected = _snapshot; _owner = owner; if (!_dirty) _conflict = false;
        }
        _screen = _screen || screen;
        if (screen && !_draft.cropped) { _draft.region = {0, 0, width, height}; _draft.cropped = true; }
        return AppResult::Success();
    }

    AppResult Update(const std::string& owner, TransformSettings value)
    {
        if (!OwnedBy(owner)) return AppResult::Failure("No edit transaction owned by this connection");
        std::string error;
        if (!Validate(value, error)) return AppResult::Failure(error);
        _draft = value; _service.SetTransform(value.transform);
        return AppResult::Success();
    }

    AppResult Apply(const std::string& owner, bool overwrite = false)
    {
        if (!OwnedBy(owner)) return AppResult::Failure("No edit transaction owned by this connection");
        auto result = Save(_draft, _expected, overwrite, false);
        if (result.ok) { _owner.clear(); _screen = false; }
        return result;
    }

    AppResult Cancel(const std::string& owner)
    {
        if (!OwnedBy(owner)) return AppResult::Failure("No edit transaction owned by this connection");
        _owner.clear(); _screen = false; _conflict = false;
        _service.SetTransform(_runtime.transform);
        return AppResult::Success();
    }
    void Disconnected(const std::string& owner) { if (OwnedBy(owner)) Cancel(owner); }
    void AbortScreenEdit()
    {
        if (Editing()) { auto owner = _owner; Cancel(owner); _error = "Screen changed; edit cancelled"; }
    }

    AppResult Reset(const std::string& owner, bool overwrite = false)
    {
        if (Editing() && !OwnedBy(owner)) return AppResult::Failure("Another client is editing transforms");
        auto result = Save({}, Editing() ? _expected : _snapshot, overwrite, true);
        if (result.ok) { _owner.clear(); _screen = false; }
        return result;
    }

    void SyncRuntime() { if (!Editing()) _runtime.transform = _service.Snapshot().transform; }
    void ScheduleSave(double now, double delay)
    {
        if (Editing()) return;
        SyncRuntime(); _dirty = true; _saveAt = now + delay;
    }
    void PauseAutosave() { _saveAt = 0; }
    AppResult Retry() { SyncRuntime(); return Save(_runtime, _snapshot, false, false); }
    AppResult ReloadAndDiscard(const std::string& owner)
    {
        if (Editing() && !OwnedBy(owner)) return AppResult::Failure("Another client is editing transforms");
        ConfigStore::Snapshot latest; TransformSettings settings;
        if (!_store.Read(latest, _error) || !Decode(latest, settings, _error)) return AppResult::Failure(_error);
        _snapshot = latest; _runtime = _disk = settings; _owner.clear();
        _dirty = _conflict = _screen = false; _saveAt = 0; ++_revision; _error.clear();
        _service.SetTransform(settings.transform);
        return AppResult::Success();
    }
    void Tick(double now)
    {
        if (now >= _reloadAt) { _reloadAt = now + .5; Reload(); }
        if (!Editing() && _saveAt > 0 && now >= _saveAt) { _saveAt = 0; Retry(); }
    }
    void Reload()
    {
        ConfigStore::Snapshot latest; TransformSettings settings;
        std::string error;
        if (!_store.Read(latest, error) || !Decode(latest, settings, error)) { _error = error; _loadFailed = true; return; }
        if (_loadFailed) { _error.clear(); _loadFailed = false; }
        if (latest.values == _snapshot.values) return;
        bool changed = false;
        for (const auto* key : {"model_x", "model_y", "model_scale", "model_region"})
            if (Get(latest, key) != Get(_snapshot, key)) changed = true;
        // Preserve the old comparison snapshot while unsaved local changes exist.
        if (_dirty && !Editing() && changed) { _conflict = true; _error = "External transform changes conflict with unsaved settings"; return; }
        _snapshot = latest; _disk = settings;
        if (changed) {
            ++_revision; _runtime = settings;
            if (Editing()) { _conflict = true; _dirty = false; _saveAt = 0; }
            else _service.SetTransform(settings.transform);
        }
    }

    // Integer pixel deltas, transformed into the model's existing coordinate system.
    static TransformSettings MoveCrop(TransformSettings value, int64_t dx, int64_t dy, int height)
    {
        auto bounded = [](int64_t n) { return int(std::max<int64_t>(INT32_MIN, std::min<int64_t>(INT32_MAX, n))); };
        int x = bounded(int64_t(value.region.x) + dx), y = bounded(int64_t(value.region.y) + dy);
        if (height > 0 && value.transform.scale > 0) {
            const double units = 2.0 / height / value.transform.scale;
            value.transform.x += (int64_t(x) - value.region.x) * units;
            value.transform.y -= (int64_t(y) - value.region.y) * units;
        }
        value.region.x = x; value.region.y = y;
        return value;
    }

private:
    static std::string Get(const ConfigStore::Snapshot& s, const std::string& key)
    {
        auto it = s.values.find(key); return it == s.values.end() ? "" : it->second;
    }
    static std::string Float(float value)
    {
        std::ostringstream out; out.imbue(std::locale::classic()); out << std::setprecision(std::numeric_limits<float>::max_digits10) << value; return out.str();
    }
    AppResult Save(const TransformSettings& settings, const ConfigStore::Snapshot& expected, bool overwrite, bool reset)
    {
        std::string error;
        if (!Validate(settings, error)) return AppResult::Failure(error);
        ConfigStore::Patch patch{{"model_x", reset ? "" : Float(settings.transform.x)},
            {"model_y", reset ? "" : Float(settings.transform.y)}, {"model_scale", reset ? "" : Float(settings.transform.scale)}, {"model_region", ""}};
        if (!reset && settings.cropped) {
            auto r = settings.region;
            patch["model_region"] = "{\"x\":" + std::to_string(r.x) + ",\"y\":" + std::to_string(r.y) + ",\"width\":" + std::to_string(r.width) + ",\"height\":" + std::to_string(r.height) + "}";
        }
        ConfigStore::Snapshot saved;
        if (!_store.Commit(patch, expected, saved, _error, overwrite)) { _conflict = _error.find("conflict") != std::string::npos; return AppResult::Failure(_error); }
        _snapshot = saved; _runtime = _disk = settings; _dirty = _conflict = false; _saveAt = 0; ++_revision;
        _service.SetTransform(settings.transform); _error.clear();
        return AppResult::Success();
    }
    ApplicationService& _service;
    ConfigStore _store;
    ConfigStore::Snapshot _snapshot, _expected;
    TransformSettings _disk, _runtime, _draft;
    std::string _owner, _error;
    bool _screen = false, _dirty = false, _conflict = false, _loadFailed = false;
    double _saveAt = 0, _reloadAt = 0;
    unsigned _revision = 0;
};
