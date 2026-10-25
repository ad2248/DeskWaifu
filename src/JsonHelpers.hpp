#pragma once

#include "ConfigStore.hpp"
#include <cmath>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

// 仅读取已校验的顶层成员。可选字段缺失时使用默认值；
// 字段存在但类型错误时，不能悄悄将其转换为合法值。
inline std::string JsonGetString(const ConfigStore::Snapshot& request, const std::string& key)
{
    const auto it = request.values.find(key);
    if (it == request.values.end()) return {};
    std::string value;
    if (!ConfigStore::ParseString(it->second, value))
        throw std::invalid_argument(key + " must be a string");
    return value;
}

inline bool JsonGetBool(const ConfigStore::Snapshot& request, const std::string& key)
{
    const auto it = request.values.find(key);
    if (it == request.values.end()) return false;
    auto value = it->second;
    // 仅保留旧版 reset/stream_source 的布尔值写法。
    if (!value.empty() && value.front() == '"') value = JsonGetString(request, key);
    if (value == "true" || value == "1") return true;
    if (value == "false" || value == "0") return false;
    throw std::invalid_argument(key + " must be boolean");
}

inline int JsonGetInt(const ConfigStore::Snapshot& request, const std::string& key, int fallback)
{
    const auto it = request.values.find(key);
    if (it == request.values.end()) return fallback;
    const auto& text = it->second;
    try {
        if (text.empty() || text.find_first_not_of("-0123456789") != std::string::npos)
            throw std::invalid_argument(key);
        size_t end;
        const auto value = std::stoll(text, &end);
        if (end != text.size() || value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
            throw std::out_of_range(key);
        return static_cast<int>(value);
    } catch (const std::exception&) {
        throw std::invalid_argument(key + " must be a 32-bit integer");
    }
}

inline float JsonGetFloat(const ConfigStore::Snapshot& request, const std::string& key, float fallback)
{
    const auto it = request.values.find(key);
    if (it == request.values.end()) return fallback;
    std::istringstream input(it->second);
    input.imbue(std::locale::classic());
    float value;
    if (!(input >> value) || !input.eof() || !std::isfinite(value))
        throw std::invalid_argument(key + " must be a finite number");
    return value;
}
