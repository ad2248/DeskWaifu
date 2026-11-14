#pragma once

#include <string>

// 空路径和绝对路径原样返回，否则直接拼接 base；调用方负责 base 的末尾分隔符。
inline std::string ResolveResourcePath(const std::string& base, const std::string& file)
{
    if (file.empty() || file.front() == '/') {
        return file;
    }
    return base + file;
}
