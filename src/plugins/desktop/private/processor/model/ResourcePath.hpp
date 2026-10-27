#pragma once

#include <string>

inline std::string ResolveResourcePath(const std::string& base, const std::string& file)
{
    if (file.empty() || file.front() == '/') return file;
    return base + file;
}
