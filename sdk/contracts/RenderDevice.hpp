// 校验用于选择渲染设备的配置字符串。
#pragma once

#include <string>

// 返回设备选择是否为默认值（空字符串或 auto）或小写 PCI 地址。
// PCI 地址格式为 dddd:bb:dd.f，设备号和功能号分别限制为 0x1f 和 7。
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
