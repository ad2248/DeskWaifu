#include "TransformEditor.hpp"
#include "CropInteraction.hpp"
#include <fstream>
#include <iostream>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { std::cerr << __LINE__ << ": " << #condition << '\n'; return 1; } } while (false)

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

int main()
{
    char directory[] = "/tmp/opencode/transform-editor-test-XXXXXX";
    CHECK(mkdtemp(directory));
    const std::string path = std::string(directory) + "/config.json";
    auto write = [&](std::string text) { std::ofstream file(path); file << text; };
    write("{\"model_x\":1,\"model_scale\":2,\"other\":true}");
    Runtime runtime; ApplicationService service(runtime);
    ConfigStore::Snapshot invalid; TransformSettings decoded; std::string validationError;
    for (const auto* document : {"{\"model_scale\":11}", "{\"model_x\":\"1\"}",
             "{\"model_region\":{\"width\":0,\"height\":1}}", "{\"model_region\":{\"width\":1.5,\"height\":1}}",
             "{\"model_region\":{\"x\":2147483648,\"width\":1,\"height\":1}}"}) {
        CHECK(ConfigStore::Parse(document, invalid, validationError));
        CHECK(!TransformEditor::Decode(invalid, decoded, validationError));
    }
    TransformEditor editor(service, path);
    CHECK(editor.Initialize({}));
    CHECK(editor.Display().transform.x == 1 && editor.Display().transform.scale == 2);
    CHECK(editor.Begin("gui", true, 1920, 1080).ok);
    CHECK(editor.Display().cropped && editor.Display().region.width == 1920);
    CHECK(editor.Display().region.height == 1080);
    CHECK(!editor.Begin("other", false, 1920, 1080).ok);
    auto draft = editor.Display(); draft.transform.x = 3; draft.region.x = 100;
    CHECK(editor.Update("gui", draft).ok);
    ConfigStore store(path); ConfigStore::Snapshot disk; std::string error;
    CHECK(store.Read(disk, error) && disk.values.at("model_x") == "1");
    editor.Disconnected("other"); CHECK(editor.Editing());
    editor.Disconnected("gui"); CHECK(!editor.Editing() && service.Snapshot().transform.x == 1);
    CHECK(editor.Begin("gui", false, 1920, 1080).ok);
    CHECK(editor.Update("gui", draft).ok);
    // External same-field change updates the committed state, not the preview.
    write("{\"model_x\":5,\"model_scale\":2,\"other\":false}");
    editor.Reload(); CHECK(editor.Conflict());
    CHECK(service.Snapshot().transform.x == 3);
    CHECK(!editor.Apply("gui").ok);
    CHECK(editor.Cancel("gui").ok && service.Snapshot().transform.x == 5);
    CHECK(editor.Begin("gui", false, 1920, 1080).ok);
    CHECK(editor.Update("gui", draft).ok);
    write("{\"model_x\":6,\"model_scale\":2,\"other\":false}");
    CHECK(editor.Apply("gui", true).ok);
    CHECK(store.Read(disk, error) && disk.values.at("model_x") == "3" && disk.values.at("other") == "false");
    // Position/zoom normal controls save only when their debounce expires.
    CHECK(service.SetTransform({1.5f, 2, -1}).ok);
    editor.ScheduleSave(10, .5); editor.Tick(10.2);
    CHECK(editor.Dirty());
    editor.Tick(10.6); CHECK(!editor.Dirty());
    CHECK(store.Read(disk, error) && disk.values.at("model_scale") == "1.5");
    TransformEditor restarted(service, path); CHECK(restarted.Initialize({}));
    CHECK(restarted.Display().transform.y == -1);
    CHECK(editor.Reset("gui").ok);
    CHECK(!editor.Display().cropped && service.Snapshot().transform.scale == 1 && service.Snapshot().transform.x == 0);
    CHECK(store.Read(disk, error) && disk.values.size() == 1 && disk.values.count("other"));
    const auto oldRevision = editor.Revision();
    write("{\"model_x\":8,\"other\":false}");
    CHECK(!editor.Begin("gui", false, 1920, 1080, oldRevision).ok);
    CHECK(!editor.Editing() && service.Snapshot().transform.x == 8);
    CHECK(service.SetTransform({1, 9, 0}).ok);
    editor.ScheduleSave(20, .5);
    write("{\"model_x\":10,\"other\":false}");
    editor.Tick(21);
    CHECK(editor.Dirty() && editor.Conflict() && service.Snapshot().transform.x == 9);
    CHECK(editor.ReloadAndDiscard("gui").ok);
    CHECK(!editor.Dirty() && service.Snapshot().transform.x == 10);
    CHECK(editor.Reset("gui").ok);
    // Failed saves retain preview and never report a successful reset.
    CHECK(editor.Begin("gui", true, 1920, 1080).ok);
    CHECK(editor.Update("gui", draft).ok);
    write("broken"); CHECK(!editor.Reset("gui").ok && editor.Editing());
    CHECK(service.Snapshot().transform.x == 3);
    editor.AbortScreenEdit(); CHECK(!editor.Editing() && service.Snapshot().transform.x == 0);
    // A 100px right / 50px down crop movement moves the model by exactly those pixels.
    TransformSettings start{{2, 1, 1}, true, {100, 100, 800, 600}};
    const auto moved = CropInteraction::Drag(start, CropInteraction::Move, 100, 50, 1000);
    CHECK(moved.region.x == 200 && moved.region.y == 150);
    CHECK(std::abs(moved.transform.x - 1.1f) < .00001f && std::abs(moved.transform.y - .95f) < .00001f);
    const auto resized = CropInteraction::Drag(start, CropInteraction::Left | CropInteraction::Top, 900, 900, 1000);
    CHECK(resized.region.width == 1 && resized.region.height == 1);
    CHECK(resized.transform.x == start.transform.x && resized.transform.scale == start.transform.scale);
    CHECK(CropInteraction::Hit(start.region, 500, 100) == CropInteraction::Move);
    CHECK(CropInteraction::Hit(start.region, 100, 100) == (CropInteraction::Left | CropInteraction::Top));
    start.region = {INT32_MIN, INT32_MIN, INT32_MAX, INT32_MAX};
    auto recovered = TransformEditor::MoveCrop(start, -int64_t(start.region.x), -int64_t(start.region.y), 1000);
    CHECK(recovered.region.x == 0 && recovered.region.y == 0);
    const auto extreme = CropInteraction::Drag(start, CropInteraction::Left | CropInteraction::Top, INT32_MIN, INT32_MIN, 1000);
    CHECK(extreme.region.width > 0 && extreme.region.height > 0);
    CHECK(unlink(path.c_str()) == 0 && rmdir(directory) == 0);
    return 0;
}
