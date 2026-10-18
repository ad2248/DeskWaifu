#pragma once

#include "ModelTransform.hpp"
#include <Math/CubismMatrix44.hpp>

// Keep the render-side composition order shared with the zoom invariant.
inline void ApplyModelTransform(Live2D::Cubism::Framework::CubismMatrix44& projection,
    ModelTransform transform)
{
    projection.ScaleRelative(transform.scale, transform.scale);
    projection.TranslateRelative(transform.x, transform.y);
}

// Clip-space projection shared by drawing and pointer hit testing. The model
// matrix is deliberately excluded: Cubism's IsHit already inverts that matrix.
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
