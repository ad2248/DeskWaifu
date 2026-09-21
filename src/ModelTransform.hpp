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

    // Keep the model point under the cursor at the same screen position.
    const float positionDelta = 1.0f / result.scale - 1.0f / current.scale;
    result.x += anchorX * positionDelta;
    result.y += anchorY * positionDelta;
    return result;
}
