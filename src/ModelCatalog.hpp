#pragma once

#include <string>
#include <vector>

struct ModelDescriptor
{
    std::string id;       // canonical directory + JSON filename; file symlinks retain overlay identity
    std::string name;     // first directory alias in lexical order (not necessarily unique)
    std::string directory;
    std::string jsonName;
};

// Filesystem-only catalog: deterministic order and identity, no SDK or GPU.
class ModelCatalog
{
public:
    static std::vector<ModelDescriptor> Scan(const std::vector<std::string>& roots);
};
