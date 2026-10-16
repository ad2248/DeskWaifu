#pragma once

#include "ModelRegion.hpp"
#include <vector>

// RGBA pixels are in OpenGL bottom-to-top order; clip is already intersected
// with the surface, in surface-local coordinates.
inline std::vector<ModelRegion> BuildModelInputRegion(
    const std::vector<unsigned char>& pixels, int width, int height, ModelRegion clip)
{
    std::vector<ModelRegion> rectangles;
    const auto addRegion = [&](int x, int y, int w, int h) {
        const auto box = IntersectModelRegions({x, y, w, h}, clip);
        if (box.width > 0 && box.height > 0) rectangles.push_back(box);
    };

    // Preserve the renderer's center-pixel sampling and horizontal block merging.
    const int BLOCK_SIZE = 10;
    for (int y = 0; y < height; y += BLOCK_SIZE) {
        int start_x = -1;
        for (int x = 0; x < width; x += BLOCK_SIZE) {
            bool opaque = false;
            const int test_x = x + BLOCK_SIZE / 2;
            const int test_y = y + BLOCK_SIZE / 2;
            if (test_x < width && test_y < height) {
                const int idx = (test_y * width + test_x) * 4;
                opaque = pixels[idx + 3] > 10;
            }
            if (opaque) {
                if (start_x < 0) start_x = x;
            } else if (start_x >= 0) {
                addRegion(start_x, height - y - BLOCK_SIZE, x - start_x, BLOCK_SIZE);
                start_x = -1;
            }
        }
        if (start_x >= 0)
            addRegion(start_x, height - y - BLOCK_SIZE, width - start_x, BLOCK_SIZE);
    }
    return rectangles;
}
