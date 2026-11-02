#pragma once

#include "contracts/ModelTransform.hpp"
#include <Math/CubismMatrix44.hpp>

// 渲染端的变换组合顺序必须与缩放不变量保持一致。
inline void ApplyModelTransform(Live2D::Cubism::Framework::CubismMatrix44& projection,
    ModelTransform transform)
{
    projection.ScaleRelative(transform.scale, transform.scale);
    projection.TranslateRelative(transform.x, transform.y);
}

// 绘制和指针命中测试共用的裁剪空间投影。
// 刻意不包含模型矩阵，因为 Cubism 的 IsHit 已对其执行逆变换。
inline Live2D::Cubism::Framework::CubismMatrix44 BuildModelProjection(
    float canvasWidth, int width, int height, ModelTransform transform,
    Live2D::Cubism::Framework::CubismMatrix44* view)
{
    Live2D::Cubism::Framework::CubismMatrix44 projection;
    if (canvasWidth > 1.0f && width < height)
        projection.Scale(1.0f, static_cast<float>(width) / height);
    else
        projection.Scale(static_cast<float>(height) / width, 1.0f);
    ApplyModelTransform(projection, transform);
    if (view) projection.MultiplyByMatrix(view);
    return projection;
}
