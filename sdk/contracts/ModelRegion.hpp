// 定义模型显示区域及其裁剪、坐标转换辅助函数。
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

// 表面局部坐标，原点位于左上角。
struct ModelRegion {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// 返回两个区域的交集；任一区域无面积或不相交时返回空区域。
inline ModelRegion IntersectModelRegions(ModelRegion a, ModelRegion b)
{
    const int x = std::max(a.x, b.x);
    const int y = std::max(a.y, b.y);
    const auto right = std::min(int64_t(a.x) + a.width, int64_t(b.x) + b.width);
    const auto bottom = std::min(int64_t(a.y) + a.height, int64_t(b.y) + b.height);
    if (a.width <= 0 || a.height <= 0 || b.width <= 0 || b.height <= 0 || right <= x || bottom <= y) {
        return { };
    }
    return { x, y, static_cast<int>(right - x), static_cast<int>(bottom - y) };
}

// 未配置时返回整个非负尺寸表面，否则返回请求区域与表面的交集。
inline ModelRegion ResolveModelRegion(bool configured, ModelRegion requested, int width, int height)
{
    const ModelRegion surface { 0, 0, std::max(0, width), std::max(0, height) };
    return configured ? IntersectModelRegions(requested, surface) : surface;
}

// 将左上角原点区域转换为 OpenGL 左下角原点坐标，不裁剪区域。
inline ModelRegion ModelRegionToOpenGL(ModelRegion region, int surfaceHeight) { return { region.x, surfaceHeight - region.y - region.height, region.width, region.height }; }

// 返回区域外部的上、下、左、右四块矩形，可包含零面积矩形。
// 调用方传入的区域须已与表面范围求交。
inline std::array<ModelRegion, 4> OutsideModelRegion(ModelRegion region, int width, int height)
{
    return { { { 0, 0, width, region.y }, { 0, region.y + region.height, width, height - region.y - region.height }, { 0, region.y, region.x, region.height },
        { region.x + region.width, region.y, width - region.x - region.width, region.height } } };
}
