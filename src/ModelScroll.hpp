#pragma once

#include "ModelTransform.hpp"

// Shared with the scroll callback so coordinate conversion and null-view
// handling can be verified independently of the renderer and Wayland.
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
