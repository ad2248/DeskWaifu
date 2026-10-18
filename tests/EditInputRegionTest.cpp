#include "ModelInputRegion.hpp"
#include "QtWindowInput.hpp"
#include "ScreenCropInteraction.hpp"
#include "TestTemporaryDirectory.hpp"
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
    const auto directory = MakeTestTemporaryDirectory("edit-input-region");
    CHECK(!directory.empty());
    const auto path = directory + "/config.json";
    auto write = [&](const std::string& text) { std::ofstream file(path); file << text; };
    Runtime runtime;
    ApplicationService service(runtime);
    LAppConfig config; config.sourcePath = path;

    // Preserve the edit transaction coverage of the removed native protocol stub.
    // Actual Qt input-region commits are checked by qt_headless_smoke.py.
    for (const auto* owner : {"local-screen", "gui"}) {
        for (const auto size : {ModelRegion{0, 0, 1000, 800}, ModelRegion{0, 0, 1280, 720}}) {
            for (const auto& model : {std::vector<ModelRegion>{{110, 130, 120, 40}, {140, 170, 80, 60}},
                    std::vector<ModelRegion>{}}) {
                write("{}");
                TransformEditor editor(service, config);
                CHECK(editor.Initialize({}));
                auto frame = [&](const std::vector<ModelRegion>& expected) {
                    const auto regions = ModelFrameInputRegions(editor.ScreenEditing(), size.width, size.height, model);
                    CHECK(QtModelInputRegion(regions) == QtModelInputRegion(expected));
                    return 0;
                };
                auto begin = [&] {
                    auto result = std::string(owner) == "local-screen"
                        ? editor.BeginLocalScreenEdit(size.width, size.height)
                        : editor.Begin(owner, true, size.width, size.height);
                    CHECK(result.ok && editor.ScreenEditing());
                    auto draft = editor.Display(); draft.region = {100, 100, 400, 300};
                    CHECK(editor.Update(owner, draft).ok);
                    return 0;
                };
                auto outsideClick = [&] {
                    ScreenCropInteraction pointer;
                    pointer.Press(editor.Display(), 50, 250);
                    return pointer.Release(editor, 100, 50, 250);
                };
                CHECK(frame(model) == 0);
                CHECK(begin() == 0);
                CHECK(frame({size}) == 0);
                CHECK(outsideClick().ok && !editor.ScreenEditing());
                CHECK(frame(model) == 0);
                CHECK(begin() == 0);
                CHECK(frame({size}) == 0);
                CHECK(editor.Cancel(owner).ok && !editor.ScreenEditing());
                CHECK(frame(model) == 0);
                CHECK(begin() == 0);
                CHECK(frame({size}) == 0);
                ConfigStore store(path); ConfigStore::Snapshot disk; std::string error;
                CHECK(store.Read(disk, error));
                write("broken");
                CHECK(!outsideClick().ok && editor.ScreenEditing() && !editor.Error().empty());
                CHECK(frame({size}) == 0);
                write(disk.document);
                CHECK(outsideClick().ok && !editor.ScreenEditing());
                CHECK(frame(model) == 0);
            }
        }
    }
    CHECK(unlink(path.c_str()) == 0 && rmdir(directory.c_str()) == 0);
}
