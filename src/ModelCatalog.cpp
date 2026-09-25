#include "ModelCatalog.hpp"

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>

namespace {
bool IsDirectory(const std::string& path)
{
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

std::string Canonical(const std::string& path)
{
    char resolved[PATH_MAX];
    return realpath(path.c_str(), resolved) ? resolved : std::string();
}
}

std::vector<ModelDescriptor> ModelCatalog::Scan(const std::vector<std::string>& roots)
{
    std::vector<ModelDescriptor> result;
    const std::string suffix = ".model3.json";
    for (const auto& root : roots)
    {
        const std::string base = Canonical(root);
        if (base.empty()) continue;
        DIR* dir = opendir(base.c_str());
        if (!dir) continue;
        while (dirent* entry = readdir(dir))
        {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            const std::string modelDir = base + "/" + name + "/";
            if (!IsDirectory(modelDir)) continue;
            DIR* inner = opendir(modelDir.c_str());
            if (!inner) continue;
            while (dirent* file = readdir(inner))
            {
                const std::string json = file->d_name;
                if (json.size() <= suffix.size() ||
                    json.compare(json.size() - suffix.size(), suffix.size(), suffix) != 0)
                    continue;
                const std::string id = Canonical(modelDir + json);
                if (!id.empty()) result.push_back({id, name, modelDir, json});
            }
            closedir(inner);
        }
        closedir(dir);
    }
    std::sort(result.begin(), result.end(), [](const ModelDescriptor& a, const ModelDescriptor& b)
    {
        return a.name == b.name ? a.id < b.id : a.name < b.name;
    });
    result.erase(std::unique(result.begin(), result.end(), [](const ModelDescriptor& a, const ModelDescriptor& b)
        { return a.id == b.id; }), result.end());
    return result;
}
