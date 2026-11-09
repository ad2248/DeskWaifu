#pragma once

#include "contracts/ModelTransform.hpp"
#include "contracts/AppState.hpp"
#include "ExternalControlState.hpp"
#include <cmath>
#include <string>

// 平台和 SDK 接口：服务持有状态，仅在对应的运行时操作成功后提交变更。
class IApplicationRuntime
{
public:
    virtual ~IApplicationRuntime() = default;
    virtual int ModelIndex() const = 0;
    virtual int ModelCount() const = 0;
    virtual bool SetHidden(bool hidden) = 0;
};

// 保存应用变换、可见性与外部控制仲裁状态；不直接持有窗口或模型。
// 借用 runtime，须在其生命周期内由 GUI 交互域串行访问。
class ApplicationService
{
public:
    explicit ApplicationService(IApplicationRuntime& runtime) : _runtime(runtime) {}

    AppSnapshot Snapshot() const
    {
        return {_transform, _hidden, _runtime.ModelIndex(), _runtime.ModelCount()};
    }

    // 接受有限平移及 [0.1, 10] 内的缩放；非法输入返回错误并保持原状态。
    AppResult SetTransform(ModelTransform value)
    {
        if (!std::isfinite(value.scale) || !std::isfinite(value.x) || !std::isfinite(value.y)
            || value.scale < 0.1f || value.scale > 10.0f)
            return AppResult::Failure("transform must be finite and zoom must be 0.1..10.0");
        _transform = value;
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
    void ClearControls() { _controls.Clear(); }
    ModelFrameInput FrameInput() const { return _controls.Frame(); }

    // 运行时接受可见性变更后才提交状态；重复请求直接成功。
    AppResult SetVisibility(bool visible)
    {
        if (_hidden == !visible) return AppResult::Success();
        if (!_runtime.SetHidden(!visible)) return AppResult::Failure("visibility update failed");
        _hidden = !visible;
        return AppResult::Success();
    }

private:
    IApplicationRuntime& _runtime;
    ModelTransform _transform{1.0f, 0.0f, 0.0f};
    bool _hidden = false;
    ExternalControlState _controls;
};
