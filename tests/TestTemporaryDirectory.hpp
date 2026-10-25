#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// 使用运行器提供的临时目录，而非代理专用路径。
inline std::string MakeTestTemporaryDirectory(const std::string& prefix)
{
    const char* configured = std::getenv("TMPDIR");
    std::string root = configured && *configured ? configured : P_tmpdir;
    std::string pattern = root + "/" + prefix + "-XXXXXX";
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back(0);
    const char* created = mkdtemp(writable.data());
    return created ? std::string(created) : std::string{};
}
