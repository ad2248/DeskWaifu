#pragma once
#include "TransformEditor.hpp"
#include <vector>

// 顶部区域同时移动模型与裁剪框，其余边缘只调整裁剪尺寸。
class CropInteraction
{
public:
    enum Handle { None = 0, Move = 1, Left = 2, Right = 4, Top = 8, Bottom = 16, Model = 32 };
    static std::vector<ModelRegion> Handles(ModelRegion r)
    {
        // 只绘制可见边框；使用 int64 运算，避免合法但位于表面外的矩形发生溢出。
        auto clamp = [](int64_t n) { return int(std::max<int64_t>(INT32_MIN, std::min<int64_t>(INT32_MAX, n))); };
        return {{r.x, r.y, r.width, 8}, {r.x, clamp(int64_t(r.y) + r.height - 8), r.width, 8},
            {r.x, r.y, 8, r.height}, {clamp(int64_t(r.x) + r.width - 8), r.y, 8, r.height},
            {clamp(int64_t(r.x) + r.width / 2 - 24), r.y, std::min(48, r.width), 16}};
    }
    static int Hit(ModelRegion r, int x, int y)
    {
        int64_t right = int64_t(r.x) + r.width, bottom = int64_t(r.y) + r.height;
        if (x < int64_t(r.x) - 8 || x > right + 8 || y < int64_t(r.y) - 8 || y > bottom + 8) return None;
        if (std::abs(int64_t(x) - (int64_t(r.x) + r.width / 2)) <= 24 && y >= r.y && int64_t(y) < int64_t(r.y) + 16) return Move;
        int hit = None;
        if (std::abs(int64_t(x) - r.x) <= 8) hit |= Left;
        else if (std::abs(int64_t(x) - right) <= 8) hit |= Right;
        if (std::abs(int64_t(y) - r.y) <= 8) hit |= Top;
        else if (std::abs(int64_t(y) - bottom) <= 8) hit |= Bottom;
        return hit;
    }
    static TransformSettings Drag(TransformSettings start, int handle, int dx, int dy, int viewportHeight)
    {
        if (handle == Move) return TransformEditor::MoveCrop(start, dx, dy, viewportHeight);
        if (handle == Model) { start.transform = InputController::Drag(start.transform, dx, dy, viewportHeight); return start; }
        auto& r = start.region;
        if (handle & Left) {
            int64_t delta = std::max({int64_t(dx), int64_t(INT32_MIN) - r.x, int64_t(r.width) - INT32_MAX});
            delta = std::min({delta, int64_t(r.width) - 1, int64_t(INT32_MAX) - r.x});
            r.x = int(int64_t(r.x) + delta); r.width = int(int64_t(r.width) - delta);
        }
        if (handle & Right) r.width = int(std::max<int64_t>(1, std::min<int64_t>(INT32_MAX, int64_t(r.width) + dx)));
        if (handle & Top) {
            int64_t delta = std::max({int64_t(dy), int64_t(INT32_MIN) - r.y, int64_t(r.height) - INT32_MAX});
            delta = std::min({delta, int64_t(r.height) - 1, int64_t(INT32_MAX) - r.y});
            r.y = int(int64_t(r.y) + delta); r.height = int(int64_t(r.height) - delta);
        }
        if (handle & Bottom) r.height = int(std::max<int64_t>(1, std::min<int64_t>(INT32_MAX, int64_t(r.height) + dy)));
        return start;
    }
};
