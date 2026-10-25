#include "MouseLookSettings.hpp"
#include "HoverOpacitySettings.hpp"
#include "TestTemporaryDirectory.hpp"
#include <fstream>
#include <iostream>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "Failed at line " << __LINE__ << ": " << #condition << '\n'; return 1; } } while (0)

int main()
{
    const auto directory = MakeTestTemporaryDirectory("deskwaifu-mouse-look");
    CHECK(!directory.empty());
    const auto path = directory + "/custom.json";
    struct Cleanup { std::string path, directory; ~Cleanup() { std::remove(path.c_str()); rmdir(directory.c_str()); } } cleanup{path, directory};
    LAppConfig config; config.sourcePath = path;
    MouseLookSettings settings(config);
    CHECK(settings.Refresh().ok && settings.Radius() == .5f);
    CHECK(settings.Set(R"({"radius":0.25})").ok && settings.Radius() == .25f);
    ConfigStore store(path);
    ConfigStore::Snapshot disk;
    std::string error;
    CHECK(store.Read(disk, error));
    const auto saved = disk.document;
    for (const auto value : {"-0.1", "1e999", "3.5e38", "null", "true", "\"0.5\"", "[]", "{}", "NaN"}) {
        CHECK(!settings.Set("{\"radius\":" + std::string(value) + "}").ok);
        CHECK(settings.Radius() == .25f);
        CHECK(store.Read(disk, error) && disk.document == saved);
        ConfigStore::Snapshot invalid;
        LAppConfig::Values values;
        if (ConfigStore::Parse("{\"mouse_look_radius\":" + std::string(value) + "}", invalid, error))
            CHECK(!LAppConfig::Decode(invalid, values, error));
    }
    CHECK(!settings.Set("{}").ok);
    CHECK(!settings.Set(R"({"radius":0.2,"radius":0.3})").ok);
    for (const auto value : {"0", "0.5", "1", "2", "5e-1"}) CHECK(settings.Set("{\"radius\":" + std::string(value) + "}").ok);
    CHECK(settings.Radius() == .5f);
    // 其他独立配置应保留已保存的视线半径，反之亦然。
    HoverOpacitySettings hover(config);
    CHECK(hover.Set(R"({"idle_opacity":0.75})").ok);
    CHECK(settings.Set(R"({"radius":0.3})").ok);
    CHECK(config.values.idleOpacity == .75f);
    LAppConfig restarted; CHECK(restarted.LoadFromFile(path));
    CHECK(restarted.values.mouseLookRadius == .3f && restarted.values.idleOpacity == .75f);
    // 文档无效或写入失败时，不能应用未保存的值。
    { std::ofstream file(path); file << "{invalid"; }
    CHECK(!settings.Refresh().ok && !settings.Set(R"({"radius":0.8})").ok);
    CHECK(settings.Radius() == .3f);
    CHECK(std::remove(path.c_str()) == 0 && mkdir(path.c_str(), 0700) == 0);
    CHECK(!settings.Set(R"({"radius":0.8})").ok && settings.Radius() == .3f);
    CHECK(rmdir(path.c_str()) == 0);
    { std::ofstream file(path); file << "{}"; }
    CHECK(settings.Refresh().ok && settings.Radius() == .5f);
    auto invalidValues = config.values;
    std::string document;
    invalidValues.mouseLookRadius = std::numeric_limits<float>::infinity();
    CHECK(!LAppConfig::Serialize(invalidValues, document, error));
    invalidValues.mouseLookRadius = -1;
    CHECK(!LAppConfig::Serialize(invalidValues, document, error));
}
