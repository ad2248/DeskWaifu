#include "CursorLook.hpp"
#include <cmath>

int main()
{
    CursorLook cursor;
    const ModelTransform model{1.0f, 0.0f, 0.0f};
    if (cursor.HasPosition()) return 1;
    if (cursor.LookAt(0, 0, 200, 200, .5f, .5f, model) !=
        std::make_pair(0.0f, 0.0f)) return 1;

    // No model hit test or pointer capture is needed, including on another screen.
    cursor.GlobalPosition(-300, 500);
    const auto outside = cursor.LookAt(-200, 100, 200, 200, .5f, .5f, model);
    if (outside != std::make_pair(-2.0f, -3.0f)) return 1;
    // A stationary cursor remains usable every frame, after a tap or transform edit.
    if (cursor.LookAt(-200, 100, 200, 200, .5f, .5f, model) != outside) return 1;
    const auto moved = cursor.LookAt(-200, 100, 200, 200, .5f, .5f, {1, 1, 1});
    if (moved != std::make_pair(-3.0f, -4.0f)) return 1;
    const auto switched = cursor.LookAt(-400, 400, 200, 200, .5f, .5f, model);
    if (switched != std::make_pair(0.0f, 0.0f)) return 1;
    // Surface events remain the fallback when global cursor capability is absent.
    cursor.LocalPosition(150, 50);
    if (cursor.LookAt(-400, 400, 200, 200, .5f, .5f, model) !=
        std::make_pair(.5f, .5f)) return 1;
    return 0;
}
