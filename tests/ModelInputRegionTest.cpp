#include "ModelInputRegion.hpp"
#include <iostream>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "Failed at line " << __LINE__ << ": " << #condition << '\n'; return 1; } } while (0)

bool Equal(ModelRegion a, ModelRegion b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

int main()
{
    // Non-block-aligned dimensions exercise both partial rows and columns.
    const int width = 27, height = 27;
    std::vector<unsigned char> pixels(width * height * 4, 255);
    const auto full = ExtractModelInputRegion(pixels.data(), width, height, {0, 0, width, height});
    CHECK(full.size() == 3);
    CHECK(Equal(full[0], {0, 17, 27, 10}));
    CHECK(Equal(full[1], {0, 7, 27, 10}));
    CHECK(Equal(full[2], {0, 0, 27, 7}));

    for (const auto requested : {ModelRegion{3, 2, 5, 4}, ModelRegion{9, 6, 2, 13},
        ModelRegion{-3, -2, 8, 6}, ModelRegion{25, 25, 8, 9},
        ModelRegion{27, 0, 4, 9}, ModelRegion{0, 27, 9, 4},
        ModelRegion{-8, 0, 4, 9}, ModelRegion{0, -8, 9, 4}, ModelRegion{}})
    {
        const auto clip = ResolveModelRegion(true, requested, width, height);
        const auto rectangles = ExtractModelInputRegion(pixels.data(), width, height, requested);
        for (const auto box : rectangles) {
            CHECK(box.width > 0 && box.height > 0);
            CHECK(Equal(IntersectModelRegions(box, clip), box));
            CHECK(Equal(IntersectModelRegions(box, {0, 0, width, height}), box));
        }
        // On an opaque image these sampled blocks cover exactly the clip,
        // without any input rectangles outside it or overlapping each other.
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            int count = 0;
            for (const auto box : rectangles)
                count += x >= box.x && x < box.x + box.width
                    && y >= box.y && y < box.y + box.height;
            const bool inside = x >= clip.x && x < clip.x + clip.width
                && y >= clip.y && y < clip.y + clip.height;
            CHECK(count == (inside ? 1 : 0));
        }
        if (clip.width == 0 || clip.height == 0) CHECK(rectangles.empty());
    }

    // Transparent gaps split horizontal runs; alpha 10 is transparent, 11 is not.
    pixels.assign(width * height * 4, 0);
    CHECK(ExtractModelInputRegion(pixels.data(), width, height, {0, 0, width, height}).empty());
    pixels[(5 * width + 5) * 4 + 3] = 11;
    pixels[(5 * width + 15) * 4 + 3] = 10;
    pixels[(5 * width + 23) * 4 + 3] = 255;
    pixels[(15 * width + 5) * 4 + 3] = 255;
    pixels[(15 * width + 15) * 4 + 3] = 255;
    const auto sparse = ExtractModelInputRegion(pixels.data(), width, height, {0, 0, width, height});
    CHECK(sparse.size() == 3);
    CHECK(Equal(sparse[0], {0, 17, 10, 10}));
    CHECK(Equal(sparse[1], {20, 17, 7, 10}));
    CHECK(Equal(sparse[2], {0, 7, 20, 10}));

    // Partial edge blocks use their actual dimensions for sampling and Y conversion.
    pixels.assign(23 * 24 * 4, 255);
    const auto shortEdges = ExtractModelInputRegion(pixels.data(), 23, 24, {0, 0, 23, 24});
    CHECK(shortEdges.size() == 3);
    CHECK(Equal(shortEdges[0], {0, 14, 23, 10}));
    CHECK(Equal(shortEdges[1], {0, 4, 23, 10}));
    CHECK(Equal(shortEdges[2], {0, 0, 23, 4}));
}
