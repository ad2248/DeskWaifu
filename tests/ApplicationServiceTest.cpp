#include "ApplicationService.hpp"
#include <limits>

struct FakeRuntime : IApplicationRuntime
{
    bool failSelection = false, failVisibility = false, failMotion = false;
    bool grabbed = false, regionEmpty = false;
    int selected = 0, calls = 0, setHiddenCalls = 0;
    bool SelectModel(int index) override
    {
        ++calls;
        if (failSelection) return false;
        selected = index;
        return true;
    }
    int ModelIndex() const override { return selected; }
    int ModelCount() const override { return 3; }
    bool SetHidden(bool hidden) override
    {
        ++setHiddenCalls;
        if (failVisibility) return false;
        if (hidden) { grabbed = false; regionEmpty = true; }
        else regionEmpty = false;
        return true;
    }
    bool PlayMotion(const std::string&, int, int) override { return !failMotion; }
    bool SetExpression(const std::string& id) override { return id == "valid"; }
    bool TriggerHotkey(const std::string& id, std::string& error) override
    { error = "bad hotkey"; return id == "valid"; }
    bool SwitchSkin() override { return true; }
};

int main()
{
    FakeRuntime runtime;
    ApplicationService service(runtime);
    const auto apply = [&](ModelTransform transform) { return service.SetTransform(transform); };
    if (!apply({2.0f, 1.0f, -1.0f}).ok) return 1;
    if (!apply({0.1f, 0.0f, 0.0f}).ok || service.Snapshot().transform.scale != 0.1f) return 1;
    if (!apply({10.0f, 0.0f, 0.0f}).ok || service.Snapshot().transform.scale != 10.0f) return 1;
    // Both configuration and IPC call the same operation: invalid partial
    // updates leave every field unchanged.
    if (apply({std::numeric_limits<float>::quiet_NaN(), 2.0f, 0.0f}).ok ||
        apply({2.0f, std::numeric_limits<float>::infinity(), 0.0f}).ok ||
        apply({11.0f, 1.0f, 0.0f}).ok) return 1;
    auto snapshot = service.Snapshot();
    if (snapshot.transform.scale != 10.0f || snapshot.transform.x != 0.0f ||
        snapshot.transform.y != 0.0f) return 1;
    if (service.SelectModel(-1).ok || runtime.calls != 0) return 1;
    runtime.failSelection = true;
    if (service.SelectModel(1).ok || snapshot.modelIndex != service.Snapshot().modelIndex) return 1;
    runtime.failSelection = false;
    if (!service.SelectModel(1).ok || service.Snapshot().modelIndex != 1) return 1;
    runtime.grabbed = true;
    runtime.failVisibility = true;
    if (service.SetVisibility(false).ok || service.Snapshot().hidden || !runtime.grabbed) return 1;
    runtime.failVisibility = false;
    if (!service.SetVisibility(false).ok || !service.Snapshot().hidden ||
        runtime.grabbed || !runtime.regionEmpty) return 1;
    const int setHiddenCalls = runtime.setHiddenCalls;
    if (!service.SetVisibility(false).ok || runtime.setHiddenCalls != setHiddenCalls) return 1;
    if (!service.SetVisibility(true).ok || service.Snapshot().hidden || runtime.regionEmpty) return 1;
    if (!service.SetVisibility(true).ok || runtime.setHiddenCalls != setHiddenCalls + 1) return 1;
    runtime.failMotion = true;
    if (service.PlayMotion("Idle", 0, 1).ok || service.SetExpression("missing").ok ||
        service.TriggerHotkey("bad").error != "bad hotkey") return 1;
    return service.SetExpression("valid").ok ? 0 : 1;
}
