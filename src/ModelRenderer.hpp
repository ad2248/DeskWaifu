#pragma once

#include "ModelFrameInput.hpp"
#include "ModelTransform.hpp"
#include <Math/CubismMatrix44.hpp>
#include <Type/csmVector.hpp>
#include <functional>

class LAppModel;

// Organizes a complete render frame; the model session owns the models and
// passes borrowed pointers, while the View supplies pre/post draw hooks.
class ModelRenderer
{
public:
    static void Draw(const Csm::csmVector<LAppModel*>& models, Csm::CubismMatrix44* view,
        int width, int height, ModelTransform transform, const ModelFrameInput& input,
        const std::function<void(LAppModel&)>& beforeDraw,
        const std::function<void(LAppModel&)>& afterDraw);
};
