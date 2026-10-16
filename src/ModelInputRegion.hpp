#pragma once

#include "ModelRegion.hpp"
#include <vector>

// Pixels are RGBA8, bottom-to-top as returned by glReadPixels. Only the
// model's own alpha belongs here, never the final hover/display opacity.
inline std::vector<ModelRegion> ExtractModelInputRegion(const unsigned char* pixels,
    int width, int height, ModelRegion clip)
{
    std::vector<ModelRegion> regions;
    if (width <= 0 || height <= 0) return regions;
    clip = IntersectModelRegions(clip, {0, 0, width, height});
    const int block = 10;
    for (int y = 0; y < height; y += block)
    {
        const int h = std::min(block, height - y);
        int start = -1;
        const auto append = [&](int end) {
            const auto box = IntersectModelRegions({start, height - y - h, end - start, h}, clip);
            if (box.width > 0 && box.height > 0) regions.push_back(box);
        };
        for (int x = 0; x < width; x += block)
        {
            const int w = std::min(block, width - x);
            const size_t sample = (size_t(y + h / 2) * width + x + w / 2) * 4;
            if (pixels[sample + 3] > 10) {
                if (start < 0) start = x;
            } else if (start >= 0) {
                append(x);
                start = -1;
            }
        }
        if (start >= 0) append(width);
    }
    return regions;
}

inline bool ModelInputRegionContains(const std::vector<ModelRegion>& regions, double x, double y)
{
    for (const auto& box : regions)
        if (x >= box.x && y >= box.y && x < int64_t(box.x) + box.width && y < int64_t(box.y) + box.height)
            return true;
    return false;
}
