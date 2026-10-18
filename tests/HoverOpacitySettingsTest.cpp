#include "HoverOpacitySettings.hpp"
#include "TestTemporaryDirectory.hpp"
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "Failed at line " << __LINE__ << ": " << #condition << '\n'; return 1; } } while (0)

int main()
{
    const auto directory = MakeTestTemporaryDirectory("deskwaifu-hover-settings");
    CHECK(!directory.empty());
    const auto path = directory + "/custom.json";
    struct Cleanup { std::string path, directory; ~Cleanup() { std::remove(path.c_str()); rmdir(directory.c_str()); } } cleanup{path, directory};
    LAppConfig config; config.sourcePath = path;
    HoverOpacitySettings settings(config);
    CHECK(settings.Refresh().ok);
    CHECK(!settings.Enabled() && settings.Opacity() == 0.5f);
    CHECK(settings.Set(R"({"enabled":true})").ok);
    CHECK(settings.Enabled() && settings.Opacity() == 0.5f);
    CHECK(access(path.c_str(), F_OK) == 0); // A missing configuration is created.
    const auto write = [&](const std::string& text) { std::ofstream file(path); file << text; };
    write("{\n // keep comment\n\"model_x\":2.5,\"extra\":\"keep\",\"hover_opacity_enabled\":true,\"hover_opacity\":0.37\n}\n");
    CHECK(settings.Refresh().ok);
    CHECK(settings.Enabled() && settings.Opacity() == 0.37f);
    CHECK(settings.Set(R"({"command":"set_hover_opacity","opacity":0.25})").ok);
    CHECK(settings.Enabled() && settings.Opacity() == 0.25f);
    ConfigStore store(path);
    ConfigStore::Snapshot disk;
    std::string error;
    CHECK(store.Read(disk, error));
    CHECK(disk.values.at("hover_opacity") == "0.25" && disk.values.at("hover_opacity_enabled") == "true");
    CHECK(disk.values.at("model_x") == "2.5" && !disk.values.count("extra"));
    CHECK(disk.values.size() == 11 && disk.values.at("window_width") == "1900");
    CHECK(disk.document.find("// keep comment") == std::string::npos);
    CHECK(settings.Set(R"({"enabled":false})").ok);
    CHECK(!settings.Enabled() && settings.Opacity() == 0.25f);
    for (const auto value : {"0", "1", "0.01", "5e-1"})
        CHECK(settings.Set("{\"opacity\":" + std::string(value) + "}").ok);
    CHECK(store.Read(disk, error));
    const auto saved = disk.document;
    for (const auto json : {R"({"opacity":-0.1})", R"({"opacity":1.00000001})", R"({"opacity":1e999})",
        R"({"opacity":"0.5"})", R"({"opacity":true})", R"({"opacity":null})", R"({"opacity":NaN})",
        R"({"enabled":1})", R"({"enabled":"true"})", R"({"enabled":null})", R"({})",
        R"({"opacity":0.2,"opacity":0.3})"}) {
        CHECK(!settings.Set(json).ok);
        CHECK(!settings.Enabled() && settings.Opacity() == 0.5f);
        CHECK(store.Read(disk, error) && disk.document == saved);
    }
    // Invalid documents and write/read failures do not publish unsaved runtime values.
    write("{invalid");
    CHECK(!settings.Set(R"({"enabled":true,"opacity":0.8})").ok);
    CHECK(!settings.Refresh().ok);
    CHECK(!settings.Enabled() && settings.Opacity() == 0.5f);
    CHECK(std::remove(path.c_str()) == 0);
    CHECK(mkdir(path.c_str(), 0700) == 0);
    CHECK(!settings.Set(R"({"enabled":true,"opacity":0.8})").ok);
    CHECK(!settings.Enabled() && settings.Opacity() == 0.5f);
    CHECK(rmdir(path.c_str()) == 0);
    write(saved);
    CHECK(settings.Set(R"({"enabled":true,"opacity":0.8})").ok);
    LAppConfig restartedConfig; restartedConfig.sourcePath = path;
    HoverOpacitySettings restarted(restartedConfig);
    CHECK(restarted.Refresh().ok);
    CHECK(restarted.Enabled() && restarted.Opacity() == 0.8f);
    // A partial update preserves another writer's current, unrelated setting.
    CHECK(store.Read(disk, error));
    ConfigStore::Snapshot external;
    auto externalValues = config.values;
    externalValues.hoverOpacityEnabled = false;
    externalValues.modelX = 3.5f;
    std::string document;
    CHECK(LAppConfig::Serialize(externalValues, document, error));
    CHECK(store.Commit(document, disk, external, error));
    CHECK(settings.Set(R"({"opacity":0.6})").ok);
    CHECK(!settings.Enabled() && settings.Opacity() == 0.6f);
    CHECK(store.Read(disk, error) && disk.values.at("model_x") == "3.5");
    CHECK(settings.IdleOpacity() == 1.f);
    for (const auto value : {"0", "1", "0.375"})
        CHECK(settings.Set("{\"idle_opacity\":" + std::string(value) + "}").ok);
    CHECK(settings.IdleOpacity() == 0.375f && !settings.Enabled() && settings.Opacity() == 0.6f);
    CHECK(restarted.Refresh().ok && restarted.IdleOpacity() == 0.375f);
    CHECK(store.Read(disk, error));
    const auto idleSaved = disk.document;
    for (const auto value : {"-0.1", "1.00000001", "1e999", "null", "true", "\"0.5\"", "[]", "{}"}) {
        CHECK(!settings.Set("{\"idle_opacity\":" + std::string(value) + ",\"enabled\":true}").ok);
        CHECK(settings.IdleOpacity() == 0.375f && !settings.Enabled());
        CHECK(store.Read(disk, error) && disk.document == idleSaved);
    }
    write("{invalid");
    CHECK(!settings.Set(R"({"idle_opacity":0.8})").ok);
    CHECK(settings.IdleOpacity() == 0.375f);
}
