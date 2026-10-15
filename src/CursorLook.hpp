#pragma once

#include "InputController.hpp"

// Retain the latest cursor sample independently of pointer capture and taps.
// Global samples stay global so output changes do not leave stale local offsets.
class CursorLook
{
public:
    void LocalPosition(double x, double y) { Set(x, y, false); }
    void GlobalPosition(double x, double y) { Set(x, y, true); }
    bool HasPosition() const { return _available; }

    std::pair<float, float> LookAt(int outputX, int outputY, int width, int height,
        float centerX, float centerY, ModelTransform model) const
    {
        if (!_available) return {0.0f, 0.0f};
        return InputController::LookAt(static_cast<float>(_x - (_global ? outputX : 0)),
            static_cast<float>(_y - (_global ? outputY : 0)), width, height,
            centerX, centerY, model);
    }

private:
    void Set(double x, double y, bool global)
    {
        _x = x; _y = y; _global = global; _available = true;
    }
    double _x = 0, _y = 0;
    bool _global = false, _available = false;
};
