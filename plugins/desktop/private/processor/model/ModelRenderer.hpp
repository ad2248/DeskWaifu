#pragma once

#include "contracts/ModelFrameInput.hpp"
#include "contracts/ModelTransform.hpp"
#include <Math/CubismMatrix44.hpp>
#include <Type/csmVector.hpp>
#include <functional>

class LAppModel;

// 组织完整的渲染帧；模型会话持有模型并传入借用指针，
// View 提供绘制前后的钩子。
class ModelRenderer
{
public:
    static void Draw(const Csm::csmVector<LAppModel*>& models, Csm::CubismMatrix44* view,
        int width, int height, ModelTransform transform, const ModelFrameInput& input,
        const std::function<void(LAppModel&)>& beforeDraw,
        const std::function<void(LAppModel&)>& afterDraw);
};
