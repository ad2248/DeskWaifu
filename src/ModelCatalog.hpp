#pragma once

#include <string>
#include <vector>

struct ModelDescriptor
{
    std::string id;       // canonical path to .model3.json
    std::string name;     // user-visible directory name (not necessarily unique)
    std::string directory;
    std::string jsonName;
};

// Filesystem-only catalog: deterministic order and identity, no SDK or GPU.
class ModelCatalog
{
public:
    static std::vector<ModelDescriptor> Scan(const std::vector<std::string>& roots);
};
