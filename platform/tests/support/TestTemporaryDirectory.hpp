#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// 使用运行器提供的临时目录，而非代理专用路径。
// 在 TMPDIR（未设置或为空时使用 P_tmpdir）下创建带 prefix 的唯一目录。
// 成功返回路径，失败返回空字符串；调用方负责清理目录及其内容。
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
