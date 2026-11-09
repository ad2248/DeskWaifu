#pragma once

#include <string>
#include <vector>

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
    static std::vector<ModelDescriptor> Scan(const std::vector<std::string>& roots);
};
