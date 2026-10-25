#pragma once

#include "ModelTransform.hpp"

// 与滚轮回调共用，以便独立于渲染器和 Wayland
// 验证坐标转换及空视图处理。
template <typename View>
ModelTransform ZoomModelFromScroll(ModelTransform current, View* view,
    float mouseX, float mouseY, float yoffset)
{
    if (!view) return current;
    const float anchorX = view->TransformScreenX(mouseX);
    const float anchorY = view->TransformScreenY(mouseY);
    const float scaleFactor = 1.0f + yoffset * 0.1f;
    return ZoomModelAroundPoint(current, anchorX, anchorY, current.scale * scaleFactor);
}
