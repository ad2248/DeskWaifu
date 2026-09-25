#pragma once

#include "ModelTransform.hpp"
#include "ExternalControlState.hpp"
#include <cmath>
#include <string>

struct AppResult
{
    bool ok;
    std::string error;
    static AppResult Success() { return {true, {}}; }
    static AppResult Failure(std::string message) { return {false, message}; }
};

struct AppSnapshot
{
    ModelTransform transform{1.0f, 0.0f, 0.0f};
    bool hidden = false;
    int modelIndex = -1;
    int modelCount = 0;
};

// Platform/SDK ports: the service owns state and only commits changes after
// the corresponding runtime operation succeeds.
class IApplicationRuntime
{
public:
    virtual ~IApplicationRuntime() = default;
    virtual bool SelectModel(int index) = 0;
    virtual int ModelIndex() const = 0;
    virtual int ModelCount() const = 0;
    virtual bool SetHidden(bool hidden) = 0;
    virtual bool PlayMotion(const std::string& group, int index, int priority) = 0;
    virtual bool SetExpression(const std::string& id) = 0;
    virtual bool TriggerHotkey(const std::string& id, std::string& error) = 0;
    virtual bool SwitchSkin() = 0;
};

class ApplicationService
{
public:
    explicit ApplicationService(IApplicationRuntime& runtime) : _runtime(runtime) {}

    AppSnapshot Snapshot() const
    {
        return {_transform, _hidden, _runtime.ModelIndex(), _runtime.ModelCount()};
    }

    AppResult SetTransform(ModelTransform value)
    {
        if (!std::isfinite(value.scale) || !std::isfinite(value.x) || !std::isfinite(value.y)
            || value.scale < 0.1f || value.scale > 10.0f)
            return AppResult::Failure("transform must be finite and zoom must be 0.1..10.0");
        _transform = value;
        return AppResult::Success();
    }

    AppResult SelectModel(int index)
    {
        if (index < 0 || index >= _runtime.ModelCount()) return AppResult::Failure("invalid index");
        const int previous = _runtime.ModelIndex();
        if (!_runtime.SelectModel(index)) return AppResult::Failure("model load failed");
        if (previous != index) _controls.Clear();
        return AppResult::Success();
    }

    AppResult SetMouth(const std::string& source, ExternalControlState::Lifetime lifetime, float value)
    {
        return _controls.SetMouth(source, lifetime, value) ? AppResult::Success()
            : AppResult::Failure("mouth must be finite and within 0..1");
    }
    AppResult SetLook(const std::string& source, ExternalControlState::Lifetime lifetime, float x, float y)
    {
        return _controls.SetLook(source, lifetime, x, y) ? AppResult::Success()
            : AppResult::Failure("look coordinates must be finite");
    }
    void ResetMouth(const std::string& source) { _controls.ResetMouth(source); }
    void ResetLook(const std::string& source) { _controls.ResetLook(source); }
    void ReleaseControls(const std::string& source) { _controls.Release(source); }
    ModelFrameInput FrameInput() const { return _controls.Frame(); }

    AppResult SetVisibility(bool visible)
    {
        if (_hidden == !visible) return AppResult::Success();
        if (!_runtime.SetHidden(!visible)) return AppResult::Failure("visibility update failed");
        _hidden = !visible;
        return AppResult::Success();
    }

    AppResult PlayMotion(const std::string& group, int index, int priority)
    {
        return _runtime.PlayMotion(group, index, priority)
            ? AppResult::Success() : AppResult::Failure("motion unavailable");
    }

    AppResult SetExpression(const std::string& id)
    {
        return _runtime.SetExpression(id)
            ? AppResult::Success() : AppResult::Failure("expression unavailable");
    }

    AppResult TriggerHotkey(const std::string& id)
    {
        std::string error;
        return _runtime.TriggerHotkey(id, error) ? AppResult::Success()
            : AppResult::Failure(error.empty() ? "hotkey unavailable" : error);
    }

    AppResult SwitchSkin()
    {
        return _runtime.SwitchSkin() ? AppResult::Success() : AppResult::Failure("no model loaded");
    }

private:
    IApplicationRuntime& _runtime;
    ModelTransform _transform{1.0f, 0.0f, 0.0f};
    bool _hidden = false;
    ExternalControlState _controls;
};
