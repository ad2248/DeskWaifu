#pragma once

struct ModelTransform
{
    float scale;
    float x;
    float y;
};

inline ModelTransform ZoomModelAroundPoint(
    ModelTransform current, float anchorX, float anchorY, float requestedScale)
{
    ModelTransform result = current;
    result.scale = requestedScale;
    if (result.scale < 0.1f) result.scale = 0.1f;
    if (result.scale > 10.0f) result.scale = 10.0f;

    if (current.scale <= 0.0f || result.scale == current.scale)
    {
        return result;
    }

    // 渲染时必须先组合缩放再组合相对平移：logical = scale * (modelPoint + translate)。
    // 参见 ModelProjection.hpp 中的 ApplyModelTransform。
    // 保持光标下的模型点在屏幕上的位置不变。
    const float positionDelta = 1.0f / result.scale - 1.0f / current.scale;
    result.x += anchorX * positionDelta;
    result.y += anchorY * positionDelta;
    return result;
}
