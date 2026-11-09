#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// 在 TMPDIR（未配置时为 P_tmpdir）创建唯一测试目录，失败返回空字符串。
// 返回路径的清理责任属于调用方，此辅助函数不自动删除目录。
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
