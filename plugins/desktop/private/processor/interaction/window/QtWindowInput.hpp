#pragma once

#include "contracts/ModelRegion.hpp"
#include <QRegion>
#include <vector>

inline QRegion QtModelInputRegion(const std::vector<ModelRegion>& regions)
{
    QRegion mask;
    for (const auto& region : regions)
        if (region.width > 0 && region.height > 0)
            mask += QRect(region.x, region.y, region.width, region.height);
    return mask;
}

// Qt Wayland 报告 XKB 键码，模型绑定使用 evdev 键码。
inline unsigned int QtWaylandModelKey(unsigned int scanCode)
{
    return scanCode >= 8 ? scanCode - 8 : 0;
}
