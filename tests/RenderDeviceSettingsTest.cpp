#include "processor/interaction/config/RenderDeviceSettings.hpp"
#include "processor/interaction/config/MouseLookSettings.hpp"
#include "TestTemporaryDirectory.hpp"
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "Failed at line " << __LINE__ << ": " << #condition << '\n'; return 1; } } while (0)

int main()
{
    const auto directory = MakeTestTemporaryDirectory("deskwaifu-render-device");
    CHECK(!directory.empty());
    const auto path = directory + "/custom.json";
    struct Cleanup { std::string path, directory; ~Cleanup() { std::remove(path.c_str()); rmdir(directory.c_str()); } } cleanup{path, directory};
    LAppConfig config; config.sourcePath = path;
    RenderDeviceSettings settings(config);
    const auto set = [&](const std::string& json) {
        ConfigStore::Snapshot request;
        std::string error;
        return ConfigStore::Parse(json, request, error) && settings.Set(request).ok;
    };
    CHECK(settings.Refresh().ok && settings.Device() == "auto");
    CHECK(set(R"({"device":"0000:00:02.0"})"));
    CHECK(settings.Device() == "0000:00:02.0");
    ConfigStore store(path);
    ConfigStore::Snapshot disk;
    std::string error;
    CHECK(store.Read(disk, error));
    const auto saved = disk.document;
    for (const auto invalid : {"42", "false", "null", "[]", "{}", "\"intel\"", "\"0000:00:02.8\""}) {
        CHECK(!set("{\"device\":" + std::string(invalid) + "}"));
        CHECK(settings.Device() == "0000:00:02.0");
        CHECK(store.Read(disk, error) && disk.document == saved);
    }
    CHECK(!set("{}") && !set(R"({"device":"auto","device":"0000:00:02.0"})"));
    MouseLookSettings look(config);
    CHECK(look.Set(R"({"radius":0.25})").ok);
    CHECK(set(R"({"device":"auto"})"));
    CHECK(config.values.mouseLookRadius == .25f && settings.Device() == "auto");
    LAppConfig restarted;
    CHECK(restarted.LoadFromFile(path));
    CHECK(restarted.values.renderDevice == "auto" && restarted.values.mouseLookRadius == .25f);
    CHECK(set(R"({"device":"0000:00:02.0"})"));
    { std::ofstream file(path); file << "{invalid"; }
    CHECK(!settings.Refresh().ok && !set(R"({"device":"auto"})"));
    CHECK(settings.Device() == "0000:00:02.0");
    CHECK(std::remove(path.c_str()) == 0 && mkdir(path.c_str(), 0700) == 0);
    CHECK(!set(R"({"device":"auto"})") && settings.Device() == "0000:00:02.0");
    CHECK(rmdir(path.c_str()) == 0);
}
