#pragma once

#include "contracts/ModelTransform.hpp"

// 返回围绕逻辑坐标锚点缩放后的变换，将缩放限制为 [0.1, 10]。
// 当前缩放为正时调整平移以保持锚点下的模型点不动；否则只更新缩放。
inline ModelTransform ZoomModelAroundPoint(ModelTransform current, float anchorX, float anchorY, float requestedScale)
{
    ModelTransform result = current;
    result.scale = requestedScale;
    if (result.scale < 0.1f) {
        result.scale = 0.1f;
    }
    if (result.scale > 10.0f) {
        result.scale = 10.0f;
    }

    if (current.scale <= 0.0f || result.scale == current.scale) {
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
