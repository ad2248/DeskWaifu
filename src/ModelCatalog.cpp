#include "ModelCatalog.hpp"

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <limits.h>
#include <set>
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
            const std::string resolved = Canonical(base + "/" + name);
            if (resolved.empty() || !IsDirectory(resolved)) continue;
            const std::string modelDir = resolved + "/";
            DIR* inner = opendir(modelDir.c_str());
            if (!inner) continue;
            while (dirent* file = readdir(inner))
            {
                const std::string json = file->d_name;
                if (json.size() <= suffix.size() ||
                    json.compare(json.size() - suffix.size(), suffix.size(), suffix) != 0)
                    continue;
                // 目录可能通过符号链接复用资源，并提供自己的热键配置。
                // 因此解析目录的真实路径，而非文件的真实路径。
                struct stat info;
                if (stat((modelDir + json).c_str(), &info) == 0 && S_ISREG(info.st_mode))
                    result.push_back({modelDir + json, name, modelDir, json});
            }
            closedir(inner);
        }
        closedir(dir);
    }
    std::sort(result.begin(), result.end(), [](const ModelDescriptor& a, const ModelDescriptor& b)
    {
        return a.name == b.name ? a.id < b.id : a.name < b.name;
    });
    // 按名称排序，确定性地选择第一个别名。其他模型可能排在别名之间，
    // 因而相同标识的条目不一定相邻。
    std::set<std::string> seen;
    result.erase(std::remove_if(result.begin(), result.end(), [&](const ModelDescriptor& model)
        { return !seen.insert(model.id).second; }), result.end());
    return result;
}
