#include "ApplicationService.hpp"
#include <limits>

struct Runtime : IApplicationRuntime
{
    int index = 0;
    int ModelIndex() const override { return index; }
    int ModelCount() const override { return 2; }
    bool SetHidden(bool) override { return true; }
};

int main()
{
    Runtime runtime;
    ApplicationService app(runtime); // non-IPC source uses identical API
    using Lifetime = ExternalControlState::Lifetime;
    if (!app.SetMouth("cli", Lifetime::Persistent, 0.3f).ok ||
        !app.SetLook("cli", Lifetime::Persistent, 0.1f, 0.2f).ok) return 1;
    auto input = app.FrameInput();
    if (!input.hasMouth || input.mouth != 0.3f || !input.hasLook || input.lookX != 0.1f) return 1;
    app.SetMouth("audio", Lifetime::Stream, 0.8f);
    app.SetLook("tracker", Lifetime::Stream, -0.5f, 0.4f);
    app.SetLook("tracker2", Lifetime::Stream, 0.7f, 0.6f);
    input = app.FrameInput();
    if (input.mouth != 0.8f || input.lookX != 0.7f) return 1;
    app.ReleaseControls("tracker2");
    if (app.FrameInput().lookX != -0.5f || app.FrameInput().mouth != 0.8f) return 1;
    app.ReleaseControls("audio"); // persistent CLI survives short connection closure
    if (app.FrameInput().mouth != 0.3f || app.FrameInput().lookX != -0.5f) return 1;
    app.ResetLook("tracker");
    if (app.FrameInput().lookX != 0.1f) return 1;
    app.ResetMouth("cli");
    if (app.FrameInput().hasMouth || !app.FrameInput().hasLook) return 1;
    if (app.SetLook("bad", Lifetime::Stream, std::numeric_limits<float>::infinity(), 0.0f).ok ||
        app.SetMouth("bad", Lifetime::Stream, 2.0f).ok) return 1;
    app.SetMouth("cli", Lifetime::Persistent, 0.4f);
    app.ClearControls(); // Called only after an actual model change completes.
    if (app.FrameInput().hasMouth || app.FrameInput().hasLook) return 1;
    app.SetMouth("cli", Lifetime::Persistent, 0.5f);
    app.ReleaseControls("cli");
    return app.FrameInput().hasMouth ? 1 : 0;
}
