#pragma once

#include "OutputController.hpp"
#include "TransformEditor.hpp"

// Display names survive reconnects, unlike Wayland registry IDs.
class OutputSettings
{
public:
    OutputSettings(IOutputBackend& backend, TransformEditor& editor, LAppConfig& config)
        : _backend(backend), _editor(editor), _config(config) {}

    AppResult Initialize()
    {
        const auto outputs = _backend.Outputs();
        if (outputs.empty()) return AppResult::Failure("No displays available");
        for (const auto& output : outputs)
            if (!output.name.empty() && output.name == _config.values.modelOutput)
                return _backend.SwitchOutput(output.id) ? AppResult::Success()
                    : AppResult::Failure("Failed to restore configured display");
        return Select(outputs.front().name);
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
};
