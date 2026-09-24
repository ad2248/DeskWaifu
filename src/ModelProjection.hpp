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
