#include "ResourcePath.hpp"
#include "ModelFrameInput.hpp"
#include "RuntimeLifecycle.hpp"

#include <iostream>
#include <vector>

int main()
{
    if (ResolveResourcePath("/app/", "shader.glsl") != "/app/shader.glsl" ||
        ResolveResourcePath("/app/", "/models/a.model3.json") != "/models/a.model3.json")
        return 1;

    // A substitute model consumes exactly the input provided by the host;
    // neither a Delegate nor an IPC socket is constructed.
    int mouthCalls = 0, lookCalls = 0;
    float mouth = 0.0f, lookX = 0.0f, lookY = 0.0f;
    const auto onMouth = [&](float value) { ++mouthCalls; mouth = value; };
    const auto onLook = [&](float x, float y) { ++lookCalls; lookX = x; lookY = y; };
    DispatchModelFrameInput({}, onMouth, onLook);
    DispatchModelFrameInput({true, 0.75f, true, -0.25f, 0.5f}, onMouth, onLook);
    if (mouthCalls != 1 || lookCalls != 1 || mouth != 0.75f || lookX != -0.25f || lookY != 0.5f)
        return 1;
    DispatchModelFrameInput({false, 0.0f, true, 0.0f, 0.0f}, onMouth, onLook);
    if (mouthCalls != 1 || lookCalls != 2) return 1;

    // Initialization can stop before the view exists; teardown still keeps
    // the owning service alive until every borrower has been released.
    std::vector<int> order;
    ReleaseRuntimeInOrder(
        [&] { order.push_back(1); }, [&] { order.push_back(2); },
        [&] { order.push_back(3); }, [&] { order.push_back(4); },
        [&] { order.push_back(5); }, [&] { order.push_back(6); });
    if (order != std::vector<int>({1, 2, 3, 4, 5, 6})) return 1;
    order.clear();
    ReleaseRuntimeInOrder(
        [&] { order.push_back(1); }, [] {}, [] {},
        [&] { order.push_back(4); }, [] {}, [&] { order.push_back(6); });
    if (order != std::vector<int>({1, 4, 6})) return 1;
    std::cout << "explicit component inputs and paths passed\n";
}
