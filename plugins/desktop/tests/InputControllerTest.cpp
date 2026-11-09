#include "processor/interaction/InputController.hpp"
#include <cmath>
#include <string>
#include <vector>

int main()
{
    const auto centered = InputController::LookAt(100, 100, 200, 200, 0.5f, 0.5f,
        {1.0f, 0.0f, 0.0f});
    if (centered.first != 0.0f || centered.second != 0.0f) return 1;
    const auto moved = InputController::Drag({1.0f, 0.0f, 0.0f}, 10, 10, 200);
    if (std::fabs(moved.x - 0.1f) > 0.00001f || std::fabs(moved.y + 0.1f) > 0.00001f) return 1;
    const auto invalid = InputController::Drag({1.0f, 0.25f, 0.5f}, 10, 10, 0);
    if (invalid.x != 0.25f || invalid.y != 0.5f) return 1;
    if (!InputController::IsTap(0, 0, 9, 9) || InputController::IsTap(0, 0, 10, 0)) return 1;
    InputController input;
    int taps = 0;
    const auto tap = [&](int x, int y) { if (x == 11 && y == 11) ++taps; };
    input.Press(10, 10);
    if (!input.IsCaptured() || input.Release(100, 11, 11, tap) !=
        InputController::ReleaseResult::TapPending) return 1;
    input.Press(10, 10);
    if (input.Release(200, 11, 11, tap) != InputController::ReleaseResult::DoubleClick || taps) return 1;
    input.Press(10, 10);
    if (input.Release(500, 30, 10, tap) != InputController::ReleaseResult::Drag || taps) return 1;
    input.Press(10, 10);
    input.Release(600, 11, 11, tap);
    if (!input.TapExpired(1000)) return 1;
    input.FlushTap(tap);
    input.FlushTap(tap);
    if (taps != 1) return 1;
    struct Binding { bool enabled = true; std::string reason, id; std::vector<std::string> keys; };
    const std::vector<Binding> bindings;
    if (input.Key(true, KEY_ESC, true, &bindings).action != ModelKeyDecision::ReleaseGrab) return 1;
    return 0;
}
