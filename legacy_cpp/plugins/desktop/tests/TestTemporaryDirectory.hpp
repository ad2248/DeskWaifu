#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// 在 TMPDIR（未设置时用系统临时目录）创建唯一目录，失败返回空字符串。
// prefix 应为不含路径分隔符的名称；调用方负责清理目录和其中的文件。
inline std::string MakeTestTemporaryDirectory(const std::string& prefix)
{
    const char* configured = std::getenv("TMPDIR");
    std::string root = configured && *configured ? configured : P_tmpdir;
    std::string pattern = root + "/" + prefix + "-XXXXXX";
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back(0);
    const char* created = mkdtemp(writable.data());
    return created ? std::string(created) : std::string { };
}
