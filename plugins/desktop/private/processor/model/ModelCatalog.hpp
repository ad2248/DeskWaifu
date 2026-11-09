#pragma once

#include <string>
#include <vector>

// 一个模型配置文件的稳定标识和加载位置；名称可能与其他配置重复。
struct ModelDescriptor
{
    std::string id;       // 真实目录路径加 JSON 文件名；文件符号链接保留覆盖目录的标识
    std::string name;     // 按字典序排列的第一个目录别名，不保证唯一
    std::string directory;
    std::string jsonName;
};

// 仅依赖文件系统的目录索引：顺序和标识确定，不依赖 SDK 或 GPU。
class ModelCatalog
{
public:
    // 扫描各根目录的直接子目录，按名称和标识排序并去重；跳过不可访问项。
    static std::vector<ModelDescriptor> Scan(const std::vector<std::string>& roots);
};
