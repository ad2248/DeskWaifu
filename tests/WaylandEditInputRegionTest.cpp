#include "LAppWayland.hpp"
#include "LAppWaylandRegion.hpp"
#include "ScreenCropInteraction.hpp"
#include "TestTemporaryDirectory.hpp"
#include <cstdarg>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { std::cerr << __LINE__ << ": " << #condition << '\n'; return 1; } } while (false)

namespace {
int compositorToken, surfaceToken, regionToken;
auto* compositor = reinterpret_cast<wl_compositor*>(&compositorToken);
auto* surface = reinterpret_cast<wl_surface*>(&surfaceToken);
auto* region = reinterpret_cast<wl_region*>(&regionToken);
std::vector<ModelRegion> building, pending, committed;
bool regionAlive = false, hasPending = false;
int commits = 0, updates = 0, destroyed = 0;

bool Same(const std::vector<ModelRegion>& a, const std::vector<ModelRegion>& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].x != b[i].x || a[i].y != b[i].y ||
            a[i].width != b[i].width || a[i].height != b[i].height) return false;
    return true;
}

void Require(bool condition)
{
    if (!condition) { std::cerr << "Unexpected Wayland request\n"; std::abort(); }
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
}

// Intercept the generated libwayland client requests, not the production region
// functions. Model double-buffered surface state without a running compositor.
extern "C" uint32_t __wrap_wl_proxy_get_version(wl_proxy*) { return 1; }
extern "C" wl_proxy* __wrap_wl_proxy_marshal_flags(wl_proxy* proxy, uint32_t opcode,
    const wl_interface* interface, uint32_t, uint32_t flags, ...)
{
    va_list args;
    va_start(args, flags);
    wl_proxy* result = nullptr;
    if (proxy == reinterpret_cast<wl_proxy*>(compositor) && opcode == WL_COMPOSITOR_CREATE_REGION) {
        Require(!regionAlive && interface == &wl_region_interface);
        building.clear(); regionAlive = true;
        result = reinterpret_cast<wl_proxy*>(region);
    } else if (proxy == reinterpret_cast<wl_proxy*>(region)) {
        Require(regionAlive);
        if (opcode == WL_REGION_ADD) {
            const int x = va_arg(args, int), y = va_arg(args, int);
            const int width = va_arg(args, int), height = va_arg(args, int);
            building.push_back({x, y, width, height});
        } else {
            Require(opcode == WL_REGION_DESTROY && (flags & WL_MARSHAL_FLAG_DESTROY));
            regionAlive = false; building.clear(); ++destroyed;
        }
    } else if (proxy == reinterpret_cast<wl_proxy*>(surface) && opcode == WL_SURFACE_SET_INPUT_REGION) {
        Require(regionAlive && va_arg(args, wl_region*) == region);
        pending = building; hasPending = true; ++updates;
    } else if (proxy == reinterpret_cast<wl_proxy*>(surface) && opcode == WL_SURFACE_COMMIT) {
        Require(!regionAlive && hasPending);
        committed = pending; hasPending = false; ++commits;
    } else Require(false);
    va_end(args);
    return result;
}

int main()
{
    const auto directory = MakeTestTemporaryDirectory("wayland-edit-input-region");
    CHECK(!directory.empty());
    const auto path = directory + "/config.json";
    auto write = [&](const std::string& text) { std::ofstream file(path); file << text; };
    Runtime runtime;
    ApplicationService service(runtime);
    LAppConfig config; config.sourcePath = path;
    WaylandContext wl; wl.compositor = compositor; wl.surface = surface;

    // Exercise local and GUI transactions, non-square output sizes, sparse model
    // rectangles, and an empty model (fully click-through outside editing).
    for (const auto* owner : {"local-screen", "gui"}) {
        for (const auto size : {ModelRegion{0, 0, 1000, 800}, ModelRegion{0, 0, 1280, 720}}) {
            for (const auto& model : {std::vector<ModelRegion>{{110, 130, 120, 40}, {140, 170, 80, 60}},
                    std::vector<ModelRegion>{}}) {
                write("{}");
                TransformEditor editor(service, config);
                CHECK(editor.Initialize({}));
                wl.width = size.width; wl.height = size.height;
                auto frame = [&](const std::vector<ModelRegion>& expected) {
                    const auto previous = committed;
                    const auto before = commits;
                    UpdateWaylandFrameInputRegion(&wl, editor.ScreenEditing(), wl.width, wl.height, model);
                    CHECK(hasPending && !regionAlive);
                    CHECK(updates == before + 1 && destroyed == updates);
                    CHECK(Same(pending, expected));
                    CHECK(Same(committed, previous) && commits == before);
                    // EGL's window-surface swap commits the pending input region.
                    // Explicitly issue that protocol request in this headless stub.
                    wl_surface_commit(wl.surface);
                    CHECK(!hasPending && commits == before + 1);
                    CHECK(Same(committed, expected));
                    return 0;
                };
                auto begin = [&] {
                    auto result = std::string(owner) == "local-screen"
                        ? editor.BeginLocalScreenEdit(wl.width, wl.height)
                        : editor.Begin(owner, true, wl.width, wl.height);
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
                CHECK(frame({size}) == 0); // Entry captures outside the crop/model.
                CHECK(outsideClick().ok && !editor.ScreenEditing());
                CHECK(frame(model) == 0); // Successful outside-click save.

                CHECK(begin() == 0);
                CHECK(frame({size}) == 0);
                CHECK(editor.Cancel(owner).ok && !editor.ScreenEditing());
                CHECK(frame(model) == 0); // Cancellation restores click-through.

                CHECK(begin() == 0);
                CHECK(frame({size}) == 0);
                ConfigStore store(path); ConfigStore::Snapshot disk; std::string error;
                CHECK(store.Read(disk, error));
                write("broken");
                CHECK(!outsideClick().ok && editor.ScreenEditing() && !editor.Error().empty());
                CHECK(frame({size}) == 0); // Failed save must retain outside input.
                write(disk.document);
                CHECK(outsideClick().ok && !editor.ScreenEditing());
                CHECK(frame(model) == 0); // Retry restores the model input region.
            }
        }
    }
    CHECK(unlink(path.c_str()) == 0 && rmdir(directory.c_str()) == 0);
    return 0;
}
