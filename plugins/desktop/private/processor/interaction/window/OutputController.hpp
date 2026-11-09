#pragma once

#include "contracts/ModelTransform.hpp"
#include <cstdint>
#include <string>
#include <vector>

// 当前后端会话内的输出标识；持久化选择应使用显示器名称。
using OutputId = std::uint32_t;

// 输出设备的值快照，几何信息采用桌面全局逻辑像素。
struct OutputSnapshot
{
    OutputId id;
    std::string name;
    int x, y, width, height;
};

// 输出枚举与切换边界；返回值快照不借用后端内部容器。
class IOutputBackend
{
public:
    virtual ~IOutputBackend() = default;
    virtual std::vector<OutputSnapshot> Outputs() const = 0;
    virtual OutputId CurrentOutput() const = 0;
    virtual bool SwitchOutput(OutputId id) = 0;
};

// 在借用的后端上执行输出选择及跨屏模型重定位，不持久化配置。
class OutputController
{
public:
    explicit OutputController(IOutputBackend& backend) : _backend(backend) {}
    // 返回容器元素的借用指针，未找到返回 nullptr；容器变动后指针可能失效。
    static const OutputSnapshot* Find(const std::vector<OutputSnapshot>& outputs, OutputId id);
    // 返回包含全局逻辑坐标的输出，未找到时返回 fallback。
    static OutputId At(const std::vector<OutputSnapshot>& outputs, int x, int y, OutputId fallback);
    // 按名称查找输出，未找到时返回 fallback。
    static OutputId Named(const std::vector<OutputSnapshot>& outputs, const std::string& name, OutputId fallback);
    // 根据输出高度调整缩放，并将模型原点移至目标输出内的全局光标位置。
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
