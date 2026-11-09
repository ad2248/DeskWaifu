#pragma once

#include "InputController.hpp"
#include <cmath>

// 独立保存最新光标采样，不受指针捕获和点击影响。
// 全局采样始终保持全局坐标，避免切换输出设备后残留旧的局部偏移。
class CursorLook
{
public:
    void LocalPosition(double x, double y) { Set(x, y, false); }
    void GlobalPosition(double x, double y) { Set(x, y, true); }
    bool HasPosition() const { return _available; }

    std::pair<float, float> LookAt(int outputX, int outputY, int width, int height,
        float centerX, float centerY, ModelTransform model, float radius = 0.5f) const
    {
        if (!_available) return {0.0f, 0.0f};
        const auto look = InputController::LookAt(static_cast<float>(_x - (_global ? outputX : 0)),
            static_cast<float>(_y - (_global ? outputY : 0)), width, height,
            centerX, centerY, model);
        const double length = std::hypot(double(look.first), double(look.second));
        if (length > radius) {
            const double scale = radius / length;
            return {static_cast<float>(look.first * scale), static_cast<float>(look.second * scale)};
        }
        return look;
    }

private:
    void Set(double x, double y, bool global)
    {
        _x = x; _y = y; _global = global; _available = true;
    }
    double _x = 0, _y = 0;
    bool _global = false, _available = false;
};
