#include "ApplicationService.hpp"
#include <limits>

struct FakeRuntime : IApplicationRuntime
{
    bool failVisibility = false;
    bool grabbed = false, regionEmpty = false;
    int selected = 0, setHiddenCalls = 0;
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
};

int main()
{
    FakeRuntime runtime;
    ApplicationService service(runtime);
    const auto apply = [&](ModelTransform transform) { return service.SetTransform(transform); };
    if (!apply({2.0f, 1.0f, -1.0f}).ok) return 1;
    if (!apply({0.1f, 0.0f, 0.0f}).ok || service.Snapshot().transform.scale != 0.1f) return 1;
    if (!apply({10.0f, 0.0f, 0.0f}).ok || service.Snapshot().transform.scale != 10.0f) return 1;
    // 配置和 IPC 调用同一个操作；无效的局部更新不能改变任何字段。
    if (apply({std::numeric_limits<float>::quiet_NaN(), 2.0f, 0.0f}).ok ||
        apply({2.0f, std::numeric_limits<float>::infinity(), 0.0f}).ok ||
        apply({11.0f, 1.0f, 0.0f}).ok ||
        apply({0.09f, 1.0f, 0.0f}).ok ||
        apply({0.0f, 1.0f, 0.0f}).ok) return 1;
    auto snapshot = service.Snapshot();
    if (snapshot.transform.scale != 10.0f || snapshot.transform.x != 0.0f ||
        snapshot.transform.y != 0.0f) return 1;
    runtime.selected = 1; // 由操作处理器提交的只读模型快照。
    if (service.Snapshot().modelIndex != 1) return 1;
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
    return 0;
}
