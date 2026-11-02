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
        if (model->GetModel()->GetCanvasWidth() > 1.0f && width < height)
        {
            model->GetModelMatrix()->SetWidth(2.0f);
        }
        auto projection = BuildModelProjection(model->GetModel()->GetCanvasWidth(),
            width, height, transform, view);
        beforeDraw(*model);
        model->Update(input);
        model->Draw(projection);
        afterDraw(*model);
    }
    offscreen->EndFrameProcess();
    offscreen->ReleaseStaleRenderTextures();
}
