#include "OutputController.hpp"
#include "DesktopWindowEvents.hpp"
#include <vector>

struct FakeBackend : IOutputBackend
{
    std::vector<OutputSnapshot> outputs{{17, "left", 0, 0, 1920, 1080},
        {42, "right", 1920, 0, 2560, 1440}};
    OutputId current = 17;
    bool fails = false;
    std::vector<OutputSnapshot> Outputs() const override { return outputs; }
    OutputId CurrentOutput() const override { return current; }
    bool SwitchOutput(OutputId id) override
    {
        if (fails || !OutputController::Find(outputs, id)) return false;
        current = id;
        return true;
    }
};

struct FakeEvents : IDesktopWindowEvents
{
    int moves = 0, keys = 0, closed = 0;
    void PointerEntered(double, double) override {}
    void PointerLeft() override {}
    void PointerMoved(double, double) override { ++moves; }
    void PointerButton(int, bool) override {}
    void PointerScroll(double) override {}
    void KeyboardChanged() override {}
    void KeyboardKey(std::uint32_t, bool) override { ++keys; }
    void SurfaceClosed() override { ++closed; }
};

int main()
{
    FakeBackend backend;
    OutputController policy(backend);
    ModelTransform transform{1.0f, 0.0f, 0.0f};
    if (OutputController::At(backend.outputs, 1919, 0, 17) != 17 ||
        OutputController::At(backend.outputs, 1920, 0, 17) != 42 ||
        OutputController::At(backend.outputs, -1, 0, 17) != 17) return 1;
    if (!policy.MoveTo(3200, 720, transform) || backend.current != 42 ||
        transform.scale != 0.75f || transform.x != 0 || transform.y != 0) return 1;
    backend.fails = true;
    if (policy.MoveTo(500, 400, transform) || backend.current != 42 || transform.x != 0) return 1;
    backend.fails = false;
    if (!policy.Focus("left") || backend.current != 17 || policy.Focus("missing")) return 1;
    // Stable registry IDs are not vector positions; removal is explicit.
    backend.outputs.erase(backend.outputs.begin());
    if (policy.Current(backend.outputs) || policy.Focus("left")) return 1;
    backend.current = 42; // backend reconciled current output after removal
    if (!policy.Current(backend.outputs) || policy.Current(backend.outputs)->id != 42) return 1;
    backend.outputs.push_back({53, "tall", 4480, 0, 1080, 2160});
    transform = {9.0f, 0.0f, 0.0f};
    if (!policy.MoveTo(5020, 1080, transform) || transform.scale != 6.0f) return 1;
    transform = OutputController::Reposition({9.0f, 0.0f, 0.0f},
        {1, "big", 0, 0, 3840, 2160}, {2, "small", 0, 0, 100, 100}, 50, 50);
    if (transform.scale != 10.0f) return 1; // clamp before service validation
    FakeEvents events;
    events.PointerMoved(3, 4);
    events.KeyboardKey(12, true);
    events.SurfaceClosed();
    return events.moves == 1 && events.keys == 1 && events.closed == 1 ? 0 : 1;
}
