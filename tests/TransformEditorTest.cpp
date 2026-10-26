#include "processor/interaction/TransformEditor.hpp"
#include "processor/interaction/CropInteraction.hpp"
#include "processor/interaction/ScreenCropInteraction.hpp"
#include "TestTemporaryDirectory.hpp"
#include <fstream>
#include <iostream>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { std::cerr << __LINE__ << ": " << #condition << '\n'; return 1; } } while (false)

struct Runtime : IApplicationRuntime
{
    int ModelIndex() const override { return 0; }
    int ModelCount() const override { return 1; }
    bool SetHidden(bool) override { return true; }
};

int main()
{
    const std::string directory = MakeTestTemporaryDirectory("transform-editor-test");
    CHECK(!directory.empty());
    if (const char* root = std::getenv("TMPDIR"))
        CHECK(directory.find(std::string(root) + "/") == 0);
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
    LAppConfig config; config.sourcePath = path;
    TransformEditor editor(service, config);
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
    // 外部对同一字段的修改更新已提交状态，不更新预览。
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
    CHECK(disk.values.size() == 15 && disk.values.at("window_width") == "1900");
    CHECK(config.values.modelX == 3 && config.values.modelRegion);
    // 普通位置和缩放控件只在防抖期结束后保存。
    CHECK(service.SetTransform({1.5f, 2, -1}).ok);
    editor.ScheduleSave(10, .5); editor.Tick(10.2);
    CHECK(editor.Dirty());
    editor.Tick(10.6); CHECK(!editor.Dirty());
    CHECK(store.Read(disk, error) && disk.values.at("model_scale") == "1.5");
    TransformEditor restarted(service, config); CHECK(restarted.Initialize({}));
    CHECK(restarted.Display().transform.y == -1);
    CHECK(editor.Reset("gui").ok);
    CHECK(!editor.Display().cropped && service.Snapshot().transform.scale == 1 && service.Snapshot().transform.x == 0);
    CHECK(store.Read(disk, error) && disk.values.size() == 14 && !disk.values.count("model_region"));
    CHECK(disk.values.at("model_x") == "0" && disk.values.at("model_y") == "0" && disk.values.at("model_scale") == "1");
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
    // 保存失败时保留预览，不能将重置报告为成功。
    CHECK(editor.Begin("gui", true, 1920, 1080).ok);
    CHECK(editor.Update("gui", draft).ok);
    write("broken"); CHECK(!editor.Reset("gui").ok && editor.Editing());
    CHECK(service.Snapshot().transform.x == 3);
    editor.AbortScreenEdit(); CHECK(!editor.Editing() && service.Snapshot().transform.x == 0);
    // 裁剪框向右移动 100 像素、向下移动 50 像素时，模型也应移动相同像素距离。
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

    // 普通点击不进入编辑；首次真实拖动转交原始按下坐标，
    // 不丢失位移，也不重复应用位移。
    write("{}");
    CHECK(editor.ReloadAndDiscard("gui").ok);
    ScreenCropInteraction pointer;
    CHECK(!pointer.StartModelDrag(editor, 100, 100, 109, 109, 1000, 1000));
    CHECK(!editor.Editing() && service.Snapshot().transform.x == 0);
    CHECK(pointer.StartModelDrag(editor, 100, 100, 150, 120, 1000, 1000));
    CHECK(editor.LocalScreenEditing() && editor.Display().cropped);
    CHECK(editor.Display().region.width == 1000 && editor.Display().region.x == 0);
    CHECK(std::abs(service.Snapshot().transform.x - .1f) < .00001f);
    pointer.Move(editor, 200, 150, 1000);
    CHECK(std::abs(service.Snapshot().transform.x - .2f) < .00001f);
    CHECK(pointer.Release(editor, 100, 200, 150).ok);
    CHECK(editor.ScreenEditing()); // 释放操作不会保存或隐藏边框。
    CHECK(store.Read(disk, error) && disk.values.empty());
    CHECK(!editor.Begin("gui", true, 1000, 1000).ok);
    CHECK(!editor.Update("gui", {}).ok && !editor.Apply("gui").ok);
    editor.Disconnected("gui"); CHECK(editor.LocalScreenEditing());

    // 双击模型内部不会保存边框，也不会触发模型动作。
    for (uint32_t time : {200u, 300u}) {
        pointer.Press(editor.Display(), 200, 200);
        CHECK(pointer.Release(editor, time, 200, 200).ok);
    }
    CHECK(editor.Editing());
    // 调整尺寸后即使回到按下位置，也不能算作点击。
    pointer.Press(editor.Display(), 0, 200);
    CHECK(pointer.Release(editor, 400, 0, 200).ok);
    pointer.Press(editor.Display(), 0, 200);
    pointer.Move(editor, 20, 200, 1000);
    pointer.Move(editor, 0, 200, 1000);
    CHECK(pointer.Release(editor, 500, 0, 200).ok);
    CHECK(editor.Editing());
    pointer.Press(editor.Display(), 0, 200);
    CHECK(pointer.Release(editor, 600, 0, 200).ok);
    CHECK(editor.Editing());
    pointer.Press(editor.Display(), 0, 200);
    CHECK(pointer.Release(editor, 700, 0, 200).ok);
    CHECK(!editor.Editing() && editor.Display().cropped);
    CHECK(store.Read(disk, error) && disk.values.count("model_region"));
    CHECK(std::abs(service.Snapshot().transform.x - .2f) < .00001f);

    // 保留现有裁剪。本地入口不能抢占 GUI 事务；
    // 同样的边框双击可结束 GUI 持有的屏幕编辑事务。
    CHECK(editor.Begin("gui", true, 1000, 1000).ok);
    CHECK(!pointer.StartModelDrag(editor, 100, 100, 150, 100, 1000, 1000));
    CHECK(editor.OwnedBy("gui"));
    for (uint32_t time : {1000u, 1100u}) {
        pointer.Press(editor.Display(), 500, 0);
        CHECK(pointer.Release(editor, time, 500, 0).ok);
    }
    CHECK(!editor.Editing());
    CHECK(pointer.StartModelDrag(editor, 100, 100, 150, 100, 500, 500));
    CHECK(editor.Display().region.width == 1000);
    CHECK(pointer.Release(editor, 1200, 150, 100).ok);
    // 外部编辑同一字段会使保存失败，但不能丢弃草稿。
    const auto preview = editor.Display();
    write("{\"model_x\":9}");
    editor.Reload();
    pointer.Press(editor.Display(), 0, 200);
    CHECK(pointer.Release(editor, 1300, 0, 200).ok);
    pointer.Press(editor.Display(), 0, 200);
    CHECK(!pointer.Release(editor, 1400, 0, 200).ok);
    CHECK(editor.LocalScreenEditing() && editor.Conflict() && !editor.Error().empty());
    CHECK(editor.Display().transform.x == preview.transform.x);
    // 改变输出尺寸或切换输出设备时，应像面板编辑一样取消本地编辑。
    editor.AbortScreenEdit();
    CHECK(!editor.ScreenEditing() && service.Snapshot().transform.x == 9);

    // 临时写入或解析失败后，可通过再次双击边框重试。
    pointer = ScreenCropInteraction{};
    CHECK(store.Read(disk, error));
    const auto savedDocument = disk.document;
    CHECK(pointer.StartModelDrag(editor, 100, 100, 120, 100, 1000, 1000));
    CHECK(pointer.Release(editor, 1500, 120, 100).ok);
    write("broken");
    pointer.Press(editor.Display(), 0, 200);
    CHECK(pointer.Release(editor, 1600, 0, 200).ok);
    pointer.Press(editor.Display(), 0, 200);
    CHECK(!pointer.Release(editor, 1700, 0, 200).ok);
    CHECK(editor.LocalScreenEditing());
    write(savedDocument);
    pointer.Press(editor.Display(), 0, 200);
    CHECK(pointer.Release(editor, 1800, 0, 200).ok);
    pointer.Press(editor.Display(), 0, 200);
    CHECK(pointer.Release(editor, 1900, 0, 200).ok);
    CHECK(!editor.Editing() && editor.Display().cropped && editor.Error().empty());

    // 本地和 GUI 屏幕编辑都通过一次区域外点击保存同一份草稿。
    for (const auto* owner : {"local-screen", "gui"}) {
        for (const auto point : {std::pair<int, int>{50, 250}, {750, 250}, {300, 50}, {300, 650}}) {
            CHECK(editor.Begin(owner, true, 1000, 1000).ok);
            auto next = editor.Display(); next.region = {100, 100, 600, 500};
            next.transform.scale = 1.5f;
            CHECK(editor.Update(owner, next).ok);
            pointer.Press(editor.Display(), point.first, point.second);
            CHECK(pointer.Release(editor, 2000, point.first, point.second).ok);
            CHECK(!editor.Editing());
            CHECK(store.Read(disk, error));
            TransformSettings saved;
            CHECK(TransformEditor::Decode(disk, saved, error));
            CHECK(saved.cropped && saved.region.x == 100 && saved.region.y == 100);
            CHECK(saved.region.width == 600 && saved.region.height == 500);
            CHECK(saved.transform.scale == 1.5f);
        }
    }

    CHECK(editor.BeginLocalScreenEdit(1000, 1000).ok);
    // 区域外拖动不会保存，即使最终回到按下位置也一样。
    pointer.Press(editor.Display(), 50, 250);
    pointer.Move(editor, 70, 250, 1000);
    pointer.Move(editor, 50, 250, 1000);
    CHECK(pointer.Release(editor, 2100, 50, 250).ok && editor.Editing());
    pointer.Press(editor.Display(), 50, 250);
    CHECK(pointer.Release(editor, 2200, 70, 250).ok && editor.Editing());
    // 在点击容差内跨过边界，不算区域外点击。
    pointer.Press(editor.Display(), 109, 250);
    CHECK(pointer.Release(editor, 2300, 99, 250).ok && editor.Editing());
    pointer.Press(editor.Display(), 91, 250);
    CHECK(pointer.Release(editor, 2400, 101, 250).ok && editor.Editing());
    // 紧邻矩形外侧的缩放热区仍属于边框手势。
    pointer.Press(editor.Display(), 95, 250);
    CHECK(pointer.Release(editor, 2500, 95, 250).ok && editor.Editing());
    pointer.Press(editor.Display(), 95, 250);
    pointer.Move(editor, 75, 250, 1000);
    CHECK(pointer.Release(editor, 2600, 75, 250).ok && editor.Editing());
    CHECK(editor.Display().region.x == 80 && editor.Display().region.width == 620);

    // 区域外点击保存失败时保留草稿，允许再次点击重试。
    CHECK(store.Read(disk, error));
    const auto beforeOutsideSave = disk.document;
    const auto outsidePreview = editor.Display();
    write("broken");
    pointer.Press(editor.Display(), 50, 250);
    CHECK(!pointer.Release(editor, 2700, 50, 250).ok);
    CHECK(editor.LocalScreenEditing() && !editor.Error().empty());
    CHECK(editor.Display().region.x == outsidePreview.region.x);
    write(beforeOutsideSave);
    pointer.Press(editor.Display(), 50, 250);
    CHECK(pointer.Release(editor, 2800, 50, 250).ok);
    CHECK(!editor.Editing() && editor.Error().empty());
    // 区域外点击也不能绕过外部编辑冲突保护。
    CHECK(editor.Begin("gui", true, 1000, 1000).ok);
    auto conflictDraft = editor.Display(); conflictDraft.transform.x += 1;
    CHECK(editor.Update("gui", conflictDraft).ok);
    write("{\"model_x\":9}");
    pointer.Press(editor.Display(), 50, 250);
    CHECK(!pointer.Release(editor, 2900, 50, 250).ok);
    CHECK(editor.OwnedBy("gui") && editor.Conflict());
    CHECK(editor.Display().transform.x == conflictDraft.transform.x);
    CHECK(store.Read(disk, error) && disk.values.at("model_x") == "9");
    CHECK(editor.Cancel("gui").ok);
    CHECK(editor.ReloadAndDiscard("gui").ok);

    // 保存当前对象的全部字段，而非仅保存变换或旧磁盘值。
    config.values.additionalModelDirs = {"~/模型", "with \"quotes\" and \\slashes"};
    config.values.defaultModel = "夕颜\nmodel";
    config.values.emotionTimeout = 7.25f;
    config.values.windowWidth = 812;
    config.values.windowHeight = 613;
    config.values.hoverOpacityEnabled = true;
    config.values.hoverOpacity = 0.375f;
    config.values.idleOpacity = 0.625f;
    CHECK(editor.Begin("gui", false, 1920, 1080).ok);
    CHECK(editor.Apply("gui").ok);
    CHECK(store.Read(disk, error) && disk.values.size() == 13);
    CHECK(disk.values.at("emotion_timeout") == "7.25");
    CHECK(disk.values.at("window_width") == "812" && disk.values.at("window_height") == "613");
    CHECK(disk.values.at("hover_opacity_enabled") == "true" && disk.values.at("hover_opacity") == "0.375");
    CHECK(disk.values.at("idle_opacity") == "0.625");
    LAppConfig loaded; CHECK(loaded.LoadFromFile(path));
    CHECK(loaded.values.additionalModelDirs == config.values.additionalModelDirs);
    CHECK(loaded.values.defaultModel == config.values.defaultModel);
    CHECK(!disk.values.count("sourcePath"));

    // 即使外部编辑的不是变换字段，也会与整个对象保存产生冲突。
    CHECK(editor.Begin("gui", false, 1920, 1080).ok);
    auto external = config.values;
    external.windowWidth = 999;
    std::string externalDocument;
    CHECK(LAppConfig::Serialize(external, externalDocument, error));
    write(externalDocument);
    CHECK(!editor.Apply("gui").ok && editor.Conflict());
    CHECK(config.values.windowWidth == 812);
    CHECK(store.Read(disk, error) && disk.values.at("window_width") == "999");
    CHECK(editor.Apply("gui", true).ok);
    CHECK(store.Read(disk, error) && disk.values.at("window_width") == "812");
    CHECK(editor.Reset("gui").ok);
    CHECK(store.Read(disk, error) && disk.values.size() == 13 && !disk.values.count("model_region"));
    CHECK(disk.values.at("idle_opacity") == "0.625");
    CHECK(disk.values.at("window_width") == "812" && disk.values.at("hover_opacity") == "0.375");
    CHECK(unlink(path.c_str()) == 0 && rmdir(directory.c_str()) == 0);
    return 0;
}
