#pragma once

#include "ModelTransform.hpp"
#include <cstdint>
#include <string>
#include <vector>

using OutputId = std::uint32_t;

struct OutputSnapshot
{
    OutputId id;
    std::string name;
    int x, y, width, height;
};

class IOutputBackend
{
public:
    virtual ~IOutputBackend() = default;
    virtual std::vector<OutputSnapshot> Outputs() const = 0;
    virtual OutputId CurrentOutput() const = 0;
    virtual bool SwitchOutput(OutputId id) = 0;
};

class OutputController
{
public:
    explicit OutputController(IOutputBackend& backend) : _backend(backend) {}
    static const OutputSnapshot* Find(const std::vector<OutputSnapshot>& outputs, OutputId id);
    static OutputId At(const std::vector<OutputSnapshot>& outputs, int x, int y, OutputId fallback);
    static OutputId Named(const std::vector<OutputSnapshot>& outputs, const std::string& name, OutputId fallback);
    static ModelTransform Reposition(ModelTransform current, const OutputSnapshot& oldOutput,
        const OutputSnapshot& nextOutput, int globalX, int globalY);

    // 缺少能力或输出设备，或后端切换失败时返回 false。
    bool MoveTo(int x, int y, ModelTransform& transform);
    bool Focus(const std::string& name);
    const OutputSnapshot* Current(const std::vector<OutputSnapshot>& outputs) const
    { return Find(outputs, _backend.CurrentOutput()); }

private:
    IOutputBackend& _backend;
};
