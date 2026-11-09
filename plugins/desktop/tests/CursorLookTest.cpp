#include "processor/interaction/CursorLook.hpp"
#include <cmath>

int main()
{
    CursorLook cursor;
    const ModelTransform model{1.0f, 0.0f, 0.0f};
    if (cursor.HasPosition()) return 1;
    if (cursor.LookAt(0, 0, 200, 200, .5f, .5f, model) !=
        std::make_pair(0.0f, 0.0f)) return 1;

    // 无需模型命中测试或指针捕获，光标位于其他屏幕时也不例外。
    cursor.GlobalPosition(-300, 500);
    const auto outside = cursor.LookAt(-200, 100, 200, 200, .5f, .5f, model);
    const auto near = [](float a, float b) { return std::abs(a - b) < 0.00001f; };
    if (!near(std::hypot(outside.first, outside.second), .5f) ||
        !near(outside.first / outside.second, 2.f / 3.f)) return 1;
    // 点击或编辑变换后，静止光标的采样仍可供每帧使用。
    if (cursor.LookAt(-200, 100, 200, 200, .5f, .5f, model) != outside) return 1;
    const auto moved = cursor.LookAt(-200, 100, 200, 200, .5f, .5f, {1, 1, 1});
    if (!near(moved.first, -.3f) || !near(moved.second, -.4f)) return 1;
    const auto switched = cursor.LookAt(-400, 400, 200, 200, .5f, .5f, model);
    if (switched != std::make_pair(0.0f, 0.0f)) return 1;
    // 缺少全局光标能力时，仍以表面事件作为回退来源。
    cursor.LocalPosition(150, 50);
    if (cursor.LookAt(-400, 400, 200, 200, .5f, .5f, model, 1.f) !=
        std::make_pair(.5f, .5f)) return 1;
    const auto diagonal = cursor.LookAt(0, 0, 200, 200, .5f, .5f, model);
    if (!near(diagonal.first, std::sqrt(.125f)) || !near(diagonal.second, std::sqrt(.125f))) return 1;
    if (cursor.LookAt(0, 0, 200, 200, .5f, .5f, model, 0) != std::make_pair(0.f, 0.f)) return 1;
    cursor.LocalPosition(130, 60);
    if (cursor.LookAt(0, 0, 200, 200, .5f, .5f, model) != std::make_pair(.3f, .4f)) return 1;
    cursor.LocalPosition(110, 90);
    if (cursor.LookAt(0, 0, 200, 200, .5f, .5f, model) != std::make_pair(.1f, .1f)) return 1;
    cursor.LocalPosition(100, 100);
    if (cursor.LookAt(0, 0, 200, 200, .5f, .5f, model, 0) != std::make_pair(0.f, 0.f)) return 1;
    return 0;
}
