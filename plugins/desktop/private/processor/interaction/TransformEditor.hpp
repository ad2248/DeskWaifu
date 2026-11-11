#pragma once

#include "ApplicationService.hpp"
#include "InputController.hpp"
#include "contracts/ConfigStore.hpp"
#include "contracts/ModelRegion.hpp"
#include "processor/interaction/config/LAppConfig.hpp"
#include <functional>
#include <limits>

// 一次显示或编辑所需的模型变换与裁剪快照；裁剪矩形使用窗口逻辑像素。
struct TransformSettings {
    ModelTransform transform { LAppConfig::Values { }.modelScale, LAppConfig::Values { }.modelX, LAppConfig::Values { }.modelY };
    bool cropped = false;
    ModelRegion region;
};

// 持有编辑事务，不负责渲染或平台策略。
// 借用服务与配置，二者须比本对象存活更久；所有操作在 GUI 交互域串行执行。
// 草稿只影响预览，成功保存才提交运行时快照；保存冲突不会自动覆盖磁盘。
class TransformEditor {
public:
    void SetEditChanged(std::function<void()> changed) { _editChanged = std::move(changed); }
    TransformEditor(ApplicationService& service, LAppConfig& config)
        : _service(service)
        , _config(config)
        , _store(config.sourcePath.empty() ? LAppConfig::GetDefaultConfigPath() : config.sourcePath)
    {
    }

    // 先应用初始变换再读取配置；初始变换非法时返回 false。
    // 磁盘配置不可读时记录错误但返回 true，保留初始状态且不覆盖无效文件。
    bool Initialize(const TransformSettings& initial)
    {
        _runtime = initial;
        if (!_service.SetTransform(initial.transform).ok) {
            return false;
        }
        ConfigStore::Snapshot snapshot;
        TransformSettings parsed;
        LAppConfig::Values values;
        if (!Read(snapshot, values, parsed, _error)) {
            _loadFailed = true;
            return true;
        } // 不覆盖无效文件。
        _config.values = std::move(values);
        _snapshot = snapshot;
        _runtime = parsed;
        _service.SetTransform(parsed.transform);
        return true;
    }

    static bool Validate(const TransformSettings& value, std::string& error)
    {
        const auto t = value.transform;
        if (!std::isfinite(t.x) || !std::isfinite(t.y) || !std::isfinite(t.scale) || t.scale < .1f || t.scale > 10.f) {
            error = "Transform must be finite; zoom must be 0.1..10";
            return false;
        }
        if (value.cropped && (value.region.width <= 0 || value.region.height <= 0)) {
            error = "Crop dimensions must be positive integers";
            return false;
        }
        return true;
    }

    static bool Decode(const ConfigStore::Snapshot& snapshot, TransformSettings& value, std::string& error)
    {
        LAppConfig::Values values;
        return Decode(snapshot, values, value, error);
    }

    static bool Decode(const ConfigStore::Snapshot& snapshot, LAppConfig::Values& values, TransformSettings& value, std::string& error)
    {
        if (!LAppConfig::Decode(snapshot, values, error)) {
            return false;
        }
        value = { { values.modelScale, values.modelX, values.modelY }, bool(values.modelRegion), values.modelRegion.value_or(ModelRegion { }) };
        auto number = [&](const char* key, float& destination) {
            auto it = snapshot.values.find(key);
            if (it == snapshot.values.end()) {
                return true;
            }
            try {
                size_t end;
                destination = std::stof(it->second, &end);
                return end == it->second.size() && std::isfinite(destination);
            } catch (...) {
                return false;
            }
        };
        if (!number("model_x", value.transform.x) || !number("model_y", value.transform.y) || !number("model_scale", value.transform.scale)) {
            error = "Invalid model transform in config";
            return false;
        }
        return Validate(value, error);
    }

    // 返回编辑草稿或已提交快照的借用引用；后续更新可能改变其内容。
    const TransformSettings& Display() const { return Editing() ? _draft : _runtime; }
    bool Editing() const { return !_owner.empty(); }
    bool OwnedBy(const std::string& owner) const { return Editing() && _owner == owner; }
    bool ScreenEditing() const { return Editing() && _screen; }
    bool LocalScreenEditing() const { return ScreenEditing() && OwnedBy("local-screen"); }
    AppResult BeginLocalScreenEdit(int width, int height)
    {
        if (Editing()) {
            return AppResult::Failure("Another client is editing transforms");
        }
        return Begin("local-screen", true, width, height);
    }
    bool Dirty() const { return _dirty; }
    bool Conflict() const { return _conflict; }
    const std::string& Error() const { return _error; }
    const std::string& Path() const { return _store.Path(); }
    const std::string& Owner() const { return _owner; }
    unsigned Revision() const { return _revision; }

    // 为非空 owner 开始或延续独占事务；其他所有者占用时失败。
    // 仅新建事务时检查 expectedRevision；同一所有者延续事务时不检查版本。
    // 首次进入屏幕编辑且尚未裁剪时，用给定窗口尺寸初始化裁剪框。
    AppResult Begin(const std::string& owner, bool screen, int width, int height, unsigned expectedRevision = std::numeric_limits<unsigned>::max())
    {
        if (owner.empty()) {
            return AppResult::Failure("Editing requires a connection");
        }
        if (Editing() && !OwnedBy(owner)) {
            return AppResult::Failure("Another client is editing transforms");
        }
        if (!Editing()) {
            Reload();
            if (expectedRevision != std::numeric_limits<unsigned>::max() && expectedRevision != _revision) {
                return AppResult::Failure("Settings changed; refresh before editing");
            }
            SyncRuntime();
            _draft = _runtime;
            _expected = _snapshot;
            _owner = owner;
            if (!_dirty) {
                _conflict = false;
            }
        }
        _screen = _screen || screen;
        if (screen && !_draft.cropped) {
            _draft.region = { 0, 0, width, height };
            _draft.cropped = true;
        }
        if (_editChanged) {
            _editChanged();
        }
        return AppResult::Success();
    }

    // 仅允许事务所有者更新有效草稿并预览模型变换，不写磁盘。
    AppResult Update(const std::string& owner, TransformSettings value)
    {
        if (!OwnedBy(owner)) {
            return AppResult::Failure("No edit transaction owned by this connection");
        }
        std::string error;
        if (!Validate(value, error)) {
            return AppResult::Failure(error);
        }
        _draft = value;
        _service.SetTransform(value.transform);
        return AppResult::Success();
    }

    // 保存所有者的草稿，成功后结束事务；失败保留草稿并返回错误。
    // overwrite 显式允许覆盖磁盘冲突，否则以开始编辑时的快照检测冲突。
    AppResult Apply(const std::string& owner, bool overwrite = false)
    {
        if (!OwnedBy(owner)) {
            return AppResult::Failure("No edit transaction owned by this connection");
        }
        auto result = Save(_draft, _expected, overwrite);
        if (result.ok) {
            _owner.clear();
            _screen = false;
            if (_editChanged) {
                _editChanged();
            }
        }
        return result;
    }

    // 仅取消指定所有者的事务，恢复已提交变换；不会改写磁盘。
    AppResult Cancel(const std::string& owner)
    {
        if (!OwnedBy(owner)) {
            return AppResult::Failure("No edit transaction owned by this connection");
        }
        _owner.clear();
        _screen = false;
        _conflict = false;
        _service.SetTransform(_runtime.transform);
        if (_editChanged) {
            _editChanged();
        }
        return AppResult::Success();
    }
    void Disconnected(const std::string& owner)
    {
        if (OwnedBy(owner)) {
            Cancel(owner);
        }
    }
    void AbortScreenEdit()
    {
        if (Editing()) {
            auto owner = _owner;
            Cancel(owner);
            _error = "Screen changed; edit cancelled";
        }
    }

    // 保存默认变换及无裁剪状态，成功后结束事务；不能抢占他人的事务。
    AppResult Reset(const std::string& owner, bool overwrite = false)
    {
        if (Editing() && !OwnedBy(owner)) {
            return AppResult::Failure("Another client is editing transforms");
        }
        auto result = Save({ }, Editing() ? _expected : _snapshot, overwrite);
        if (result.ok) {
            _owner.clear();
            _screen = false;
            if (_editChanged) {
                _editChanged();
            }
        }
        return result;
    }

    void SyncRuntime()
    {
        if (!Editing()) {
            _runtime.transform = _service.Snapshot().transform;
        }
    }
    // 将当前运行时变换标为待保存；使用与 Tick() 相同的秒时钟安排延迟提交。
    // 编辑事务存在时不安排自动保存。
    void ScheduleSave(double now, double delay)
    {
        if (Editing()) {
            return;
        }
        SyncRuntime();
        _dirty = true;
        _saveAt = now + delay;
    }
    void PauseAutosave() { _saveAt = 0; }
    AppResult Retry()
    {
        SyncRuntime();
        return Save(_runtime, _snapshot, false);
    }
    // 保存输出名称并清零平移、移除裁剪，保留缩放；编辑或配置冲突时失败。
    AppResult SaveOutput(const std::string& name)
    {
        if (Editing()) {
            return AppResult::Failure("Finish transform editing before changing display");
        }
        Reload();
        if (_loadFailed || _conflict) {
            return AppResult::Failure(_error);
        }
        SyncRuntime();
        auto settings = _runtime;
        settings.transform.x = settings.transform.y = 0;
        settings.cropped = false;
        settings.region = { };
        return Save(settings, _snapshot, false, &name);
    }
    // 重新加载磁盘设置，成功后丢弃本地草稿和待保存状态并递增版本。
    // 不能丢弃其他连接的事务；读取失败保留当前设置。
    AppResult ReloadAndDiscard(const std::string& owner)
    {
        if (Editing() && !OwnedBy(owner)) {
            return AppResult::Failure("Another client is editing transforms");
        }
        ConfigStore::Snapshot latest;
        TransformSettings settings;
        LAppConfig::Values values;
        if (!Read(latest, values, settings, _error)) {
            return AppResult::Failure(_error);
        }
        _config.values = std::move(values);
        _snapshot = latest;
        _runtime = settings;
        _owner.clear();
        _dirty = _conflict = _screen = false;
        _saveAt = 0;
        ++_revision;
        _error.clear();
        _service.SetTransform(settings.transform);
        if (_editChanged) {
            _editChanged();
        }
        return AppResult::Success();
    }
    // 按秒时钟轮询磁盘变更并执行到期的自动保存，错误留在 Error() 中。
    void Tick(double now)
    {
        if (now >= _reloadAt) {
            _reloadAt = now + .5;
            Reload();
        }
        if (!Editing() && _saveAt > 0 && now >= _saveAt) {
            _saveAt = 0;
            Retry();
        }
    }
    void Reload()
    {
        ConfigStore::Snapshot latest;
        TransformSettings settings;
        LAppConfig::Values values;
        std::string error;
        if (!Read(latest, values, settings, error)) {
            _error = error;
            _loadFailed = true;
            return;
        }
        if (_loadFailed) {
            _error.clear();
            _loadFailed = false;
        }
        if (latest.values == _snapshot.values) {
            return;
        }
        bool changed = false;
        for (const auto* key : { "model_x", "model_y", "model_scale", "model_region" }) {
            if (Get(latest, key) != Get(_snapshot, key)) {
                changed = true;
            }
        }
        // 存在未保存的本地变更时，保留原来的比较快照。
        if (_dirty && !Editing() && changed) {
            _conflict = true;
            _error = "External transform changes conflict with unsaved settings";
            return;
        }
        _config.values = std::move(values);
        _snapshot = latest;
        if (changed) {
            ++_revision;
            _runtime = settings;
            if (Editing()) {
                _conflict = true;
                _dirty = false;
                _saveAt = 0;
            } else {
                _service.SetTransform(settings.transform);
            }
        }
    }

    // 将整数像素增量转换到模型现有坐标系。
    static TransformSettings MoveCrop(TransformSettings value, int64_t dx, int64_t dy, int height)
    {
        auto bounded = [](int64_t n) { return int(std::max<int64_t>(INT32_MIN, std::min<int64_t>(INT32_MAX, n))); };
        int x = bounded(int64_t(value.region.x) + dx), y = bounded(int64_t(value.region.y) + dy);
        if (height > 0 && value.transform.scale > 0) {
            const double units = 2.0 / height / value.transform.scale;
            value.transform.x += (int64_t(x) - value.region.x) * units;
            value.transform.y -= (int64_t(y) - value.region.y) * units;
        }
        value.region.x = x;
        value.region.y = y;
        return value;
    }

private:
    static std::string Get(const ConfigStore::Snapshot& s, const std::string& key)
    {
        auto it = s.values.find(key);
        return it == s.values.end() ? "" : it->second;
    }
    bool Read(ConfigStore::Snapshot& snapshot, LAppConfig::Values& values, TransformSettings& settings, std::string& error)
    {
        return _store.Read(snapshot, error) && Decode(snapshot, values, settings, error);
    }
    AppResult Save(const TransformSettings& settings, const ConfigStore::Snapshot& expected, bool overwrite, const std::string* output = nullptr)
    {
        std::string error;
        if (!Validate(settings, error)) {
            return AppResult::Failure(error);
        }
        auto values = _config.values;
        if (output) {
            values.modelOutput = *output;
        }
        values.modelX = settings.transform.x;
        values.modelY = settings.transform.y;
        values.modelScale = settings.transform.scale;
        values.modelRegion = settings.cropped ? std::experimental::optional<ModelRegion>(settings.region) : std::experimental::nullopt;
        std::string document;
        if (!LAppConfig::Serialize(values, document, _error)) {
            return AppResult::Failure(_error);
        }
        ConfigStore::Snapshot saved;
        if (!_store.Commit(document, expected, saved, _error, overwrite)) {
            _conflict = _error.find("conflict") != std::string::npos;
            return AppResult::Failure(_error);
        }
        _config.values = std::move(values);
        _snapshot = saved;
        _runtime = settings;
        _dirty = _conflict = false;
        _saveAt = 0;
        ++_revision;
        _service.SetTransform(settings.transform);
        _error.clear();
        return AppResult::Success();
    }
    ApplicationService& _service;
    LAppConfig& _config;
    ConfigStore _store;
    ConfigStore::Snapshot _snapshot, _expected;
    TransformSettings _runtime, _draft;
    std::string _owner, _error;
    bool _screen = false, _dirty = false, _conflict = false, _loadFailed = false;
    double _saveAt = 0, _reloadAt = 0;
    unsigned _revision = 0;
    std::function<void()> _editChanged;
};
