#include "OutputController.hpp"
#include <algorithm>

const OutputSnapshot* OutputController::Find(const std::vector<OutputSnapshot>& outputs, OutputId id)
{
    for (const auto& output : outputs) if (output.id == id) return &output;
    return nullptr;
}

OutputId OutputController::At(const std::vector<OutputSnapshot>& outputs, int x, int y, OutputId fallback)
{
    for (const auto& output : outputs)
        if (output.width > 0 && output.height > 0 && x >= output.x && y >= output.y &&
            static_cast<long long>(x) < static_cast<long long>(output.x) + output.width &&
            static_cast<long long>(y) < static_cast<long long>(output.y) + output.height)
            return output.id;
    return fallback;
}

OutputId OutputController::Named(const std::vector<OutputSnapshot>& outputs,
    const std::string& name, OutputId fallback)
{
    for (const auto& output : outputs) if (output.name == name) return output.id;
    return fallback;
}

ModelTransform OutputController::Reposition(ModelTransform current, const OutputSnapshot& oldOutput,
    const OutputSnapshot& nextOutput, int globalX, int globalY)
{
    if (oldOutput.height <= 0 || nextOutput.height <= 0 || current.scale <= 0) return current;
    current.scale = std::max(0.1f, std::min(10.0f,
        current.scale * static_cast<float>(oldOutput.height) / nextOutput.height));
    current.x = (globalX - nextOutput.x - nextOutput.width * 0.5f) /
        (nextOutput.height * 0.5f) / current.scale;
    current.y = -(globalY - nextOutput.y - nextOutput.height * 0.5f) /
        (nextOutput.height * 0.5f) / current.scale;
    return current;
}

bool OutputController::MoveTo(int x, int y, ModelTransform& transform)
{
    const auto outputs = _backend.Outputs();
    const auto* previous = Current(outputs);
    if (!previous) return false;
    const OutputId target = At(outputs, x, y, previous->id);
    const auto* next = Find(outputs, target);
    if (!next) return false;
    if (target == previous->id) return true;
    if (!_backend.SwitchOutput(target)) return false;
    transform = Reposition(transform, *previous, *next, x, y);
    return true;
}

bool OutputController::Focus(const std::string& name)
{
    const auto outputs = _backend.Outputs();
    const auto current = _backend.CurrentOutput();
    const auto target = Named(outputs, name, current);
    const auto* selected = Find(outputs, target);
    return selected && selected->name == name &&
        (target == current || _backend.SwitchOutput(target));
}
