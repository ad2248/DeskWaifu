#include "HoverOpacity.hpp"
#include "LAppConfig.hpp"
#include "TestTemporaryDirectory.hpp"
#include <fstream>
#include <iostream>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "Failed at line " << __LINE__ << ": " << #condition << '\n'; return 1; } } while (0)

int main()
{
    const int width = 23, height = 19;
    std::vector<unsigned char> pixels(width * height * 4, 0);
    const auto alpha = [&](int x, int y, unsigned char value) { pixels[(y * width + x) * 4 + 3] = value; };
    // Bottom row: opaque, threshold-excluded, opaque partial block.
    alpha(5, 5, 255); alpha(15, 5, 10); alpha(21, 5, 11);
    // Top partial row: adjacent blocks merge, including the partial right edge.
    alpha(5, 14, 255); alpha(15, 14, 255); alpha(21, 14, 255);
    const auto regions = ExtractModelInputRegion(pixels.data(), width, height, {0, 0, width, height});
    CHECK(regions.size() == 3);
    CHECK(ModelInputRegionContains(regions, 0, 0));
    CHECK(ModelInputRegionContains(regions, 22.9, 18.9));
    CHECK(!ModelInputRegionContains(regions, 15, 15));
    CHECK(!ModelInputRegionContains(regions, 23, 0));
    CHECK(!ModelInputRegionContains(regions, 0, 19));
    CHECK(!ModelInputRegionContains(regions, -0.1, 0));
    const auto cropped = ExtractModelInputRegion(pixels.data(), width, height, {2, 3, 5, 4});
    CHECK(cropped.size() == 1);
    CHECK(cropped[0].x == 2 && cropped[0].y == 3 && cropped[0].width == 5 && cropped[0].height == 4);
    CHECK(ExtractModelInputRegion(pixels.data(), width, height, {100, 100, 1, 1}).empty());
    CHECK(ExtractModelInputRegion(nullptr, 0, 0, {}).empty());

    HoverOpacity hover;
    CHECK(hover.Resolve(true, 0.5f, regions) == 1);
    hover.Enter(3, 4);
    CHECK(hover.Resolve(true, 0.5f, regions) == 0.5f);
    CHECK(hover.Resolve(false, 0.5f, regions) == 1);
    for (const float opacity : {0.0f, 0.01f, 0.5f, 1.0f})
        for (int frame = 0; frame < 5; ++frame)
            CHECK(hover.Resolve(true, opacity, regions) == opacity);
    // Stationary cursor: animated/cropped/switched model may leave and return.
    CHECK(hover.Resolve(true, 0.5f, {}) == 1);
    CHECK(hover.Resolve(true, 0.5f, regions) == 0.5f);
    hover.Move(15, 15); // Still on the surface during a pointer grab, but outside the model.
    CHECK(hover.Resolve(true, 0.5f, regions) == 1);
    hover.Move(3, 4);
    hover.Leave(); // Also used for pointer capability loss, hide and surface replacement.
    CHECK(hover.Resolve(true, 0.5f, regions) == 1);
    hover.Move(3, 4);
    CHECK(hover.Resolve(true, 0.5f, regions) == 1); // Motion alone cannot restore surface focus.
    hover.Enter(3, 4);
    CHECK(hover.Resolve(true, 0.5f, regions) == 0.5f);
    const std::vector<ModelRegion> handles{{14, 14, 2, 2}};
    hover.Move(15, 15);
    CHECK(ModelInputRegionContains(handles, 15, 15));
    CHECK(hover.Resolve(true, 0.5f, regions) == 1); // Handles are not model regions.
    hover.Move(3, 4);
    CHECK(hover.Resolve(true, 0.5f, regions, {{2, 3, 3, 3}}) == 1); // Handle overlapping the model.
    CHECK(hover.Resolve(true, 0.5f, regions, handles) == 0.5f);

    const auto directory = MakeTestTemporaryDirectory("deskwaifu-hover");
    CHECK(!directory.empty());
    const std::string path = directory + "/config.json";
    struct Cleanup { std::string path, directory; ~Cleanup() { std::remove(path.c_str()); rmdir(directory.c_str()); } } cleanup{path, directory};
    LAppConfig application;
    auto& config = application.values;
    const auto load = [&](const std::string& text) {
        { std::ofstream file(path); file << text; }
        return application.LoadFromFile(path);
    };
    CHECK(load("{}"));
    CHECK(!config.hoverOpacityEnabled && config.hoverOpacity == 0.5f);
    CHECK(load("{\"hover_opacity_enabled\":true,\"hover_opacity\":0.25}"));
    CHECK(config.hoverOpacityEnabled && config.hoverOpacity == 0.25f);
    CHECK(load("{\"hover_opacity_enabled\": false,\"hover_opacity\":1e0}"));
    CHECK(!config.hoverOpacityEnabled && config.hoverOpacity == 1);
    CHECK(load("{\"hover_opacity_enabled\":true, // comment\n\"hover_opacity\":0}"));
    CHECK(config.hoverOpacityEnabled && config.hoverOpacity == 0);
    CHECK(load(R"({"metadata":{"hover_opacity_enabled":true,"hover_opacity":0},"note":"hover_opacity"})"));
    CHECK(!config.hoverOpacityEnabled && config.hoverOpacity == 0.5f);
    for (const auto value : {"-0.1", "1.1", "1e999", "NaN", "Infinity", "null", "true", "\"0.5\"", "0.5bad", "[]", "{}"})
        CHECK(!load("{\"hover_opacity\":" + std::string(value) + "}"));
    for (const auto value : {"1", "null", "\"true\"", "{}"})
        CHECK(!load("{\"hover_opacity_enabled\":" + std::string(value) + "}"));
    CHECK(load("{}"));
    CHECK(!config.hoverOpacityEnabled && config.hoverOpacity == 0.5f);
}
