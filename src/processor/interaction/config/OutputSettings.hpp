#pragma once

#include "processor/interaction/window/OutputController.hpp"
#include "processor/interaction/TransformEditor.hpp"

// 与 Wayland 注册表 ID 不同，显示器名称在重新连接后仍可使用。
class OutputSettings
{
public:
    OutputSettings(IOutputBackend& backend, TransformEditor& editor, LAppConfig& config)
        : _backend(backend), _editor(editor), _config(config) {}

    const std::string& Warning() const { return _warning; }

    AppResult Initialize()
    {
        _warning.clear();
        const auto outputs = _backend.Outputs();
        if (outputs.empty()) return AppResult::Failure("No displays available");
        for (const auto& output : outputs)
            if (!output.name.empty() && output.name == _config.values.modelOutput)
                return _backend.SwitchOutput(output.id) ? AppResult::Success()
                    : AppResult::Failure("Failed to restore configured display");
        if (!_backend.SwitchOutput(outputs.front().id)) return AppResult::Failure("Failed to select fallback display");
        // 可读取的旧配置即使无法在磁盘上升级，也必须保持可用。
        // 只有显式选择输出设备时，保存失败才回滚。
        const auto saved = _editor.SaveOutput(outputs.front().name);
        if (!saved.ok) _warning = "Could not save fallback display: " + saved.error;
        return AppResult::Success();
    }

    AppResult Select(const std::string& name)
    {
        if (_editor.Editing()) return AppResult::Failure("Finish transform editing before changing display");
        const auto outputs = _backend.Outputs();
        const OutputSnapshot* target = nullptr;
        for (const auto& output : outputs) if (!name.empty() && output.name == name) target = &output;
        if (!target) return AppResult::Failure("Display is no longer available");
        const auto previous = _backend.CurrentOutput();
        if (previous == target->id && _config.values.modelOutput == name) return AppResult::Success();
        if (!_backend.SwitchOutput(target->id)) return AppResult::Failure("Failed to switch display");
        const auto result = _editor.SaveOutput(name);
        if (!result.ok && previous != target->id && !_backend.SwitchOutput(previous))
            return AppResult::Failure(result.error + "; failed to restore previous display");
        return result;
    }

private:
    IOutputBackend& _backend;
    TransformEditor& _editor;
    LAppConfig& _config;
    std::string _warning;
};
