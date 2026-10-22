#include "LAppConfig.hpp"
#include "TestTemporaryDirectory.hpp"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "Failed at line " << __LINE__ << ": " << #condition << '\n'; return 1; } } while (0)

bool SameSettings(const LAppConfig::Values& a, const LAppConfig::Values& b)
{
    return a.additionalModelDirs == b.additionalModelDirs && a.defaultModel == b.defaultModel
        && a.emotionTimeout == b.emotionTimeout && a.modelScale == b.modelScale
        && a.modelX == b.modelX && a.modelY == b.modelY
        && a.renderDevice == b.renderDevice
        && a.windowWidth == b.windowWidth && a.windowHeight == b.windowHeight
        && a.hoverOpacityEnabled == b.hoverOpacityEnabled && a.hoverOpacity == b.hoverOpacity
        && a.idleOpacity == b.idleOpacity && a.mouseLookRadius == b.mouseLookRadius
        && bool(a.modelRegion) == bool(b.modelRegion)
        && (!a.modelRegion || (a.modelRegion->x == b.modelRegion->x && a.modelRegion->y == b.modelRegion->y
        && a.modelRegion->width == b.modelRegion->width && a.modelRegion->height == b.modelRegion->height));
}

int main()
{
    const auto directory = MakeTestTemporaryDirectory("deskwaifu-config");
    CHECK(!directory.empty());
    const std::string path = directory + "/config.json";
    struct Cleanup { std::string path, directory; ~Cleanup() { std::remove(path.c_str()); rmdir(directory.c_str()); } } cleanup{path, directory};
    const LAppConfig::Values defaults;
    LAppConfig application;
    auto& config = application.values;
    const auto load = [&](const std::string& text) {
        { std::ofstream file(path); file << text; }
        return application.LoadFromFile(path);
    };
    CHECK(load("{}"));
    CHECK(SameSettings(config, defaults));
    CHECK(application.sourcePath == path);

    const std::string configured = R"({"additional_model_dirs":["/models"],"default_model":"sample",
        "emotion_timeout":9,"model_scale":2,"model_x":3,"model_y":4,
        "window_width":800,"window_height":600,"hover_opacity_enabled":true,"hover_opacity":0.25,
        "idle_opacity":0.375,"mouse_look_radius":0.3,"model_region":{"x":1,"y":2,"width":300,"height":400}})";
    CHECK(load(configured));
    CHECK(config.additionalModelDirs == std::vector<std::string>{"/models"});
    CHECK(config.defaultModel == "sample" && config.emotionTimeout == 9);
    CHECK(config.modelScale == 2 && config.modelX == 3 && config.modelY == 4);
    CHECK(config.windowWidth == 800 && config.windowHeight == 600);
    CHECK(config.hoverOpacityEnabled && config.hoverOpacity == 0.25f);
    CHECK(config.modelRegion && config.modelRegion->x == 1 && config.modelRegion->y == 2);
    CHECK(config.modelRegion->width == 300 && config.modelRegion->height == 400);

    // Missing fields must use construction defaults, not previously loaded values.
    CHECK(load("{}"));
    CHECK(SameSettings(config, defaults));

    // Failed numeric conversions use the same defaults as missing fields.
    CHECK(load(configured));
    CHECK(load(R"({"emotion_timeout":null,"model_scale":null,"model_x":null,"model_y":null,
        "window_width":null,"window_height":null})"));
    CHECK(SameSettings(config, defaults));

    CHECK(load(configured));
    const LAppConfig::Values previous = config;
    CHECK(!load(R"({"hover_opacity":2})"));
    CHECK(SameSettings(config, previous));
    CHECK(!application.LoadFromFile(directory + "/missing.json"));
    CHECK(SameSettings(config, previous));

    // A full document contains every persisted setting, but no runtime metadata.
    std::string document, error;
    CHECK(LAppConfig::Serialize(config, document, error));
    ConfigStore::Snapshot snapshot;
    CHECK(ConfigStore::Parse(document, snapshot, error));
    CHECK(snapshot.values.size() == 13 && !snapshot.values.count("sourcePath"));
    CHECK(load(document));
    CHECK(SameSettings(config, previous));

    config.defaultModel = "模型 \"quoted\" \\ line\n\t\r";
    config.defaultModel.push_back('\0');
    config.additionalModelDirs = {"~/models", "with \\ and \" and\nnewline", "夕颜"};
    config.modelX = 0.123456789f;
    config.modelY = -0.987654321f;
    config.modelScale = 1.23456789f;
    const auto escaped = config;
    CHECK(LAppConfig::Serialize(config, document, error));
    CHECK(load(document));
    CHECK(SameSettings(config, escaped));

    CHECK(load(R"({"default_model":"\u5915\u989c\ud83d\ude00","additional_model_dirs":["~\/models"]})"));
    CHECK(config.defaultModel == "夕颜😀" && config.additionalModelDirs[0] == "~/models");
    CHECK(LAppConfig::Serialize(defaults, document, error));
    CHECK(ConfigStore::Parse(document, snapshot, error));
    CHECK(snapshot.values.size() == 12 && !snapshot.values.count("model_region"));
    CHECK(load(document) && SameSettings(config, defaults));
    CHECK(load(R"({"render_device":"0000:00:02.0"})"));
    CHECK(config.renderDevice == "0000:00:02.0");
    CHECK(LAppConfig::Serialize(config, document, error));
    CHECK(load(document) && config.renderDevice == "0000:00:02.0");
    CHECK(!load(R"({"render_device":42})"));
    CHECK(!load(R"({"render_device":"intel"})"));
    CHECK(config.renderDevice == "0000:00:02.0");
    CHECK(load(R"({"render_device":"auto"})") && config.renderDevice == "auto");
    CHECK(load("{}") && config.renderDevice.empty());
    { std::ofstream file(path); file << R"({"render_device":false})"; }
    CHECK(!application.LoadFromFile(path, &error));
    CHECK(error.find("render_device") != std::string::npos);
}
