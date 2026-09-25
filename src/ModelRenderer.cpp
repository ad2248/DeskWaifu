#include "ModelRenderer.hpp"
#include "LAppModel.hpp"
#include "LAppPal.hpp"
#include "ModelProjection.hpp"
#include <Rendering/OpenGL/CubismOffscreenManager_OpenGLES2.hpp>

using namespace Csm;

void ModelRenderer::Draw(const csmVector<LAppModel*>& models, CubismMatrix44* view,
    int width, int height, ModelTransform transform, const ModelFrameInput& input,
    const std::function<void(LAppModel&)>& beforeDraw,
    const std::function<void(LAppModel&)>& afterDraw)
{
    if (width <= 0 || height <= 0) return;
    auto* offscreen = Csm::Rendering::CubismOffscreenManager_OpenGLES2::GetInstance();
    offscreen->BeginFrameProcess();
    for (csmUint32 i = 0; i < models.GetSize(); ++i)
    {
        LAppModel* model = models[i];
        if (!model || !model->GetModel())
        {
            LAppPal::PrintLogLn("Failed to model->GetModel().");
            continue;
        }
        CubismMatrix44 projection;
        if (model->GetModel()->GetCanvasWidth() > 1.0f && width < height)
        {
            model->GetModelMatrix()->SetWidth(2.0f);
            projection.Scale(1.0f, static_cast<float>(width) / static_cast<float>(height));
        }
        else
        {
            projection.Scale(static_cast<float>(height) / static_cast<float>(width), 1.0f);
        }
        ApplyModelTransform(projection, transform);
        if (view) projection.MultiplyByMatrix(view);
        beforeDraw(*model);
        model->Update(input);
        model->Draw(projection);
        afterDraw(*model);
    }
    offscreen->EndFrameProcess();
    offscreen->ReleaseStaleRenderTextures();
}
