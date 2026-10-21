#pragma once

#include <string>

inline bool ValidRenderDevice(const std::string& device)
{
    if (device.empty() || device == "auto") return true;
    if (device.size() != 12) return false;
    for (size_t i = 0; i < device.size(); ++i) {
        if (i == 4 || i == 7) { if (device[i] != ':') return false; }
        else if (i == 10) { if (device[i] != '.') return false; }
        else if (!((device[i] >= '0' && device[i] <= '9') || (device[i] >= 'a' && device[i] <= 'f'))) return false;
    }
    return device[8] <= '1' && device[11] <= '7';
}
