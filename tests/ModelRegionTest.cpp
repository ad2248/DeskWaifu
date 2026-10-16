#include "ModelRegion.hpp"
#include "LAppConfig.hpp"
#include <climits>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <vector>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "Failed at line " << __LINE__ << ": " << #condition << '\n'; return 1; } } while (0)

bool Equal(ModelRegion a, ModelRegion b)
{
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

int main()
{
    CHECK(Equal(ResolveModelRegion(false, {}, 1920, 1080), {0, 0, 1920, 1080}));
    CHECK(Equal(ResolveModelRegion(false, {}, 1280, 720), {0, 0, 1280, 720}));
    CHECK(Equal(ResolveModelRegion(true, {100, 80, 800, 700}, 1920, 1080), {100, 80, 800, 700}));
    CHECK(Equal(ResolveModelRegion(true, {-10, -20, 100, 100}, 1920, 1080), {0, 0, 90, 80}));
    CHECK(Equal(ResolveModelRegion(true, {1900, 1000, INT_MAX, INT_MAX}, 1920, 1080), {1900, 1000, 20, 80}));
    CHECK(Equal(ResolveModelRegion(true, {INT_MAX, 0, INT_MAX, 50}, 1920, 1080), {}));
    CHECK(Equal(ResolveModelRegion(true, {0, -200, 100, 100}, 1920, 1080), {}));
    CHECK(Equal(ModelRegionToOpenGL({100, 80, 800, 700}, 1080), {100, 300, 800, 700}));
    CHECK(Equal(IntersectModelRegions({90, 70, 20, 20}, {100, 80, 800, 700}), {100, 80, 10, 10}));

    // Every pixel outside is cleared exactly once, including an empty clip.
    for (const auto clip : {ModelRegion{3, 2, 5, 4}, ModelRegion{}, ModelRegion{0, 0, 13, 9}})
    {
        const auto outside = OutsideModelRegion(clip, 13, 9);
        for (int y = 0; y < 9; ++y) for (int x = 0; x < 13; ++x)
        {
            int count = 0;
            for (const auto box : outside)
                count += x >= box.x && x < box.x + box.width && y >= box.y && y < box.y + box.height;
            const bool inside = x >= clip.x && x < clip.x + clip.width && y >= clip.y && y < clip.y + clip.height;
            CHECK(count == (inside ? 0 : 1));
        }
    }

    const char* tempDir = std::getenv("TMPDIR");
    if (!tempDir) tempDir = access("/tmp/opencode", W_OK) == 0 ? "/tmp/opencode" : "/tmp";
    const std::string pattern = std::string(tempDir) + "/deskwaifu-region-XXXXXX";
    std::vector<char> storage(pattern.begin(), pattern.end());
    storage.push_back('\0');
    const char* path = storage.data();
    const int fd = mkstemp(storage.data());
    CHECK(fd >= 0);
    close(fd);
    struct Cleanup { const char* path; ~Cleanup() { std::remove(path); } } cleanup{path};
    LAppConfig application;
    auto& config = application.values;
    const auto load = [&](const std::string& content) {
        { std::ofstream file(path); file << content; }
        return application.LoadFromFile(path);
    };
    CHECK(load("{}"));
    CHECK(!config.modelRegion);
    CHECK(load(R"({"additional_model_dirs":["model_region"],"model_scale":2.0})"));
    CHECK(!config.modelRegion);
    CHECK(config.additionalModelDirs == std::vector<std::string>{"model_region"});
    CHECK(config.modelScale == 2.0f);
    CHECK(load(R"({"default_model":"model_region","model_scale":2.0})"));
    CHECK(!config.modelRegion && config.defaultModel == "model_region");
    CHECK(load(R"({"metadata":{"model_region":{"width":50,"height":60}},"model_scale":2.0})"));
    CHECK(!config.modelRegion && config.modelScale == 2.0f);
    CHECK(load(R"({"metadata":[{"model_region":{"width":50,"height":60}}]})"));
    CHECK(!config.modelRegion);
    CHECK(load(R"({"note":"escaped \"model_region\": { and }","model_scale":2.0})"));
    CHECK(!config.modelRegion && config.modelScale == 2.0f);
    CHECK(load(R"({"metadata":{"model_region":{"width":50,"height":60}},"additional_model_dirs":["model_region"],"model_region":{"width":800,"height":700}})"));
    CHECK(config.modelRegion && Equal(*config.modelRegion, {0, 0, 800, 700}));
    CHECK(load(R"({"note":"escaped \"model_region\": { and }","model_region":{"width":800,"height":700}})"));
    CHECK(config.modelRegion && Equal(*config.modelRegion, {0, 0, 800, 700}));
    CHECK(load(R"({"model_region":{"x":100,"y":80,"width":800,"height":700},"model_x":0.5})"));
    CHECK(config.modelRegion && Equal(*config.modelRegion, {100, 80, 800, 700}));
    CHECK(config.modelX == 0.5f);
    CHECK(load("{\"model_region\": {\n\"height\":700, // comment\n\"width\":800}}"));
    CHECK(config.modelRegion && Equal(*config.modelRegion, {0, 0, 800, 700}));
    CHECK(load(R"({"model_region":{"x":-100,"width":800,"height":700}})"));
    CHECK(config.modelRegion && config.modelRegion->x == -100);
    for (const auto invalid : {
        R"({"model_region":null})", R"({"model_region":{}})",
        R"({"model_region":null,"metadata":{"model_region":{"width":50,"height":60}}})",
        R"({"metadata":{"model_region":{"width":50,"height":60}},"model_region":null})",
        R"({"model_region":{"width":0,"height":10}})",
        R"({"model_region":{"width":10,"height":-1}})",
        R"({"model_region":{"width":1.5,"height":10}})",
        R"({"model_region":{"width":"10","height":10}})",
        R"({"model_region":{"width":2147483648,"height":10}})",
        R"({"model_region":{"width":10,"height":10,}})",
        R"({"model_region":{"width":10,"height":10,"width":20}})"})
        CHECK(!load(invalid));
    CHECK(load("{}"));
    CHECK(!config.modelRegion); // Reload must reset the optional region.
}
