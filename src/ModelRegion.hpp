#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

// Surface-local coordinates, with the origin at the top left.
struct ModelRegion
{
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

inline ModelRegion IntersectModelRegions(ModelRegion a, ModelRegion b)
{
    const int x = std::max(a.x, b.x);
    const int y = std::max(a.y, b.y);
    const auto right = std::min(int64_t(a.x) + a.width, int64_t(b.x) + b.width);
    const auto bottom = std::min(int64_t(a.y) + a.height, int64_t(b.y) + b.height);
    if (a.width <= 0 || a.height <= 0 || b.width <= 0 || b.height <= 0
        || right <= x || bottom <= y)
        return {};
    return {x, y, static_cast<int>(right - x), static_cast<int>(bottom - y)};
}

inline ModelRegion ResolveModelRegion(bool configured, ModelRegion requested, int width, int height)
{
    const ModelRegion surface{0, 0, std::max(0, width), std::max(0, height)};
    return configured ? IntersectModelRegions(requested, surface) : surface;
}

inline ModelRegion ModelRegionToOpenGL(ModelRegion region, int surfaceHeight)
{
    return {region.x, surfaceHeight - region.y - region.height, region.width, region.height};
}

// The caller supplies a region already intersected with the surface.
inline std::array<ModelRegion, 4> OutsideModelRegion(ModelRegion region, int width, int height)
{
    return {{
        {0, 0, width, region.y},
        {0, region.y + region.height, width, height - region.y - region.height},
        {0, region.y, region.x, region.height},
        {region.x + region.width, region.y, width - region.x - region.width, region.height}
    }};
}
