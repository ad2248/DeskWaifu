#pragma once
#include <string>
#include <vector>

struct ModelBinding
{
    std::string id, name, action, file, reason;
    std::vector<std::string> keys;
    bool enabled = true;
    float fade = 0.5f, duration = -1;
    int expression = -1;
};
