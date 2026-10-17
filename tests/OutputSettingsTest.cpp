#include "OutputSettings.hpp"
#include "TestTemporaryDirectory.hpp"
#include <fstream>
#include <iostream>
#include <cerrno>
#include <sys/stat.h>

#define CHECK(c) do { if (!(c)) { std::cerr << __LINE__ << ": " << #c << '\n'; return 1; } } while (false)

// Permission errors must be deterministic even when CI runs with root privileges.
static bool denyConfigWrites = false;
extern "C" int __real_mkstemp(char*);
extern "C" int __wrap_mkstemp(char* name)
{
    if (denyConfigWrites) { errno = EACCES; return -1; }
    return __real_mkstemp(name);
}

struct Runtime : IApplicationRuntime
{
    bool SelectModel(int) override { return true; }
    int ModelIndex() const override { return 0; }
    int ModelCount() const override { return 1; }
    bool SetHidden(bool) override { return true; }
    bool PlayMotion(const std::string&, int, int) override { return true; }
    bool SetExpression(const std::string&) override { return true; }
    bool TriggerHotkey(const std::string&, std::string&) override { return true; }
    bool SwitchSkin() override { return true; }
};

struct Backend : IOutputBackend
{
    std::vector<OutputSnapshot> outputs{{1, "DP-1", 0, 0, 1920, 1080}, {2, "HDMI-A-1", -1280, 0, 1280, 1024}};
    OutputId current = 1;
    bool fail = false;
    std::vector<OutputSnapshot> Outputs() const override { return outputs; }
    OutputId CurrentOutput() const override { return current; }
    bool SwitchOutput(OutputId id) override { if (fail) return false; current = id; return true; }
};

int main()
{
    const auto directory = MakeTestTemporaryDirectory("output-settings-test");
    CHECK(!directory.empty());
    const auto path = directory + "/config.json";
    auto write = [&](const std::string& text) { std::ofstream(path) << text; };
    write(R"({"model_output":"HDMI-A-1","model_x":2,"model_y":-1,"model_scale":2,"model_region":{"x":10,"y":20,"width":100,"height":200}})");
    Runtime runtime; ApplicationService service(runtime);
    LAppConfig config; config.sourcePath = path;
    TransformEditor editor(service, config); CHECK(editor.Initialize({}));
    Backend backend; OutputSettings settings(backend, editor, config);
    CHECK(settings.Initialize().ok && backend.current == 2);
    CHECK(editor.Display().transform.x == 2 && editor.Display().cropped); // Restart preserves placement.
    CHECK(!settings.Select("missing").ok && backend.current == 2);
    CHECK(editor.Begin("gui", false, 1280, 1024).ok);
    CHECK(!settings.Select("DP-1").ok && backend.current == 2);
    CHECK(editor.Cancel("gui").ok);
    backend.fail = true;
    CHECK(!settings.Select("DP-1").ok && config.values.modelOutput == "HDMI-A-1");
    backend.fail = false;
    CHECK(settings.Select("DP-1").ok && backend.current == 1);
    CHECK(editor.Display().transform.x == 0 && editor.Display().transform.y == 0);
    CHECK(editor.Display().transform.scale == 2 && !editor.Display().cropped);
    CHECK(config.values.modelOutput == "DP-1");
    ConfigStore store(path); ConfigStore::Snapshot disk; std::string error;
    CHECK(store.Read(disk, error) && disk.values.at("model_output") == "\"DP-1\"");
    CHECK(!disk.values.count("model_region"));
    CHECK(editor.Reset("gui").ok);
    CHECK(config.values.modelOutput == "DP-1"); // Other transform saves retain the display choice.
    CHECK(store.Read(disk, error) && disk.values.at("model_output") == "\"DP-1\"");
    CHECK(service.SetTransform({2, 3, 4}).ok);
    CHECK(settings.Select("DP-1").ok && service.Snapshot().transform.x == 3); // Same output is a no-op.
    write("broken");
    CHECK(!settings.Select("HDMI-A-1").ok && backend.current == 1);
    CHECK(service.Snapshot().transform.x == 3 && config.values.modelOutput == "DP-1");
    write(R"({"model_output":"unplugged","model_x":5,"model_scale":1.5})");
    CHECK(editor.ReloadAndDiscard("gui").ok);
    CHECK(settings.Initialize().ok && backend.current == 1);
    CHECK(config.values.modelOutput == "DP-1" && editor.Display().transform.x == 0);
    CHECK(store.Read(disk, error) && disk.values.at("model_output") == "\"DP-1\"");
    write("{}"); CHECK(editor.ReloadAndDiscard("gui").ok);
    CHECK(settings.Initialize().ok && config.values.modelOutput == "DP-1");
    backend.outputs.clear();
    CHECK(!settings.Initialize().ok && config.values.modelOutput == "DP-1");
    CHECK(store.Read(disk, error) && disk.values.at("model_output") == "\"DP-1\"");
    for (const auto* json : {"{\"model_output\":42}", "{\"model_output\":null}"}) {
        CHECK(ConfigStore::Parse(json, disk, error));
        LAppConfig::Values values; CHECK(!LAppConfig::Decode(disk, values, error));
    }
    // An upgrade must still start from a readable legacy config in a read-only directory.
    const std::string legacy = R"({"model_x":0.25,"model_y":-0.5,"model_scale":2})";
    write(legacy);
    CHECK(editor.ReloadAndDiscard("gui").ok);
    backend.outputs = {{1, "DP-1", 0, 0, 1920, 1080}, {2, "HDMI-A-1", 1920, 0, 1280, 1024}};
    backend.current = 2;
    backend.fail = true;
    CHECK(!settings.Initialize().ok && settings.Warning().empty()); // A real display failure remains fatal.
    backend.fail = false;
    CHECK(chmod(directory.c_str(), 0555) == 0);
    denyConfigWrites = true;
    const auto startup = settings.Initialize();
    denyConfigWrites = false;
    CHECK(chmod(directory.c_str(), 0700) == 0);
    CHECK(startup.ok && backend.current == 1);
    CHECK(!settings.Warning().empty());
    CHECK(editor.Display().transform.scale == 2 && editor.Display().transform.x == .25f && editor.Display().transform.y == -.5f);
    CHECK(store.Read(disk, error) && disk.document == legacy && !disk.values.count("model_output"));
    // An explicit switch must still fail and restore the previous output on the same error.
    denyConfigWrites = true;
    const auto selected = settings.Select("HDMI-A-1");
    denyConfigWrites = false;
    CHECK(!selected.ok && backend.current == 1);
    CHECK(store.Read(disk, error) && disk.document == legacy);
    CHECK(settings.Initialize().ok && settings.Warning().empty());
    return 0;
}
