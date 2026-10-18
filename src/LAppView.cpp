/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "LAppView.hpp"
#include <math.h>
#include <string>
#include "LAppPal.hpp"
#include "LAppLive2DManager.hpp"
#include "LAppTextureManager.hpp"
#include "LAppDefine.hpp"
#include "TouchManager_Common.hpp"
#include "LAppSprite.hpp"
#include "LAppSpriteShader.hpp"
#include "LAppModel.hpp"

#include <Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp>

using namespace std;
using namespace LAppDefine;

LAppView::LAppView(LAppLive2DManager& manager, LAppTextureManager& textures) :
    LAppView_Common(),
    _manager(manager),
    _textures(textures),
    _back(NULL),
    _gear(NULL),
    _power(NULL),
    _renderSprite(NULL),
    _renderTarget(SelectTarget_None),
    _spriteShader(NULL)
{
    _clearColor[0] = 1.0f;
    _clearColor[1] = 1.0f;
    _clearColor[2] = 1.0f;
    _clearColor[3] = 0.0f;

#if defined(USE_RENDER_TARGET)
    _renderTarget = SelectTarget_ViewFrameBuffer;
#elif defined(USE_MODEL_RENDER_TARGET)
    _renderTarget = SelectTarget_ModelFrameBuffer;
#endif
    if (_renderTarget != SelectTarget_None) SetRenderTargetClearColor(0.0f, 0.0f, 0.0f);

    // タッチ関係のイベント管理
    _touchManager = new TouchManager_Common();
}

LAppView::~LAppView()
{
    _renderBuffer.DestroyRenderTarget();
   if (_renderSprite)
    {
        delete _renderSprite;
    }
    if (_spriteShader)
    {
        delete _spriteShader;
    }
    if (_touchManager)
    {
        delete _touchManager;
    }
    if (_back)
    {
        delete _back;
    }
    if (_gear)
    {
        delete _gear;
    }
    if (_power)
    {
        delete _power;
    }
}

void LAppView::Initialize(int width, int height)
{
    _width = width;
    _height = height;
    LAppView_Common::Initialize(width, height);

    // シェーダー作成
    if(_spriteShader == NULL)
    {
        _spriteShader = new LAppSpriteShader();
    }
}

void LAppView::Render(ModelTransform transform, const ModelFrameInput& input)
{
    // Disable background and UI to make it a transparent desktop pet
    // _back->Render(_width, _height);
    // _gear->Render(_width, _height);
    // _power->Render(_width, _height);

    _manager.SetViewMatrix(_viewMatrix);

    // Cubism更新・描画
    _manager.OnUpdate(_width, _height, transform, input,
        [this](LAppModel& model) { PreModelDraw(model); },
        [this](LAppModel& model) { PostModelDraw(model); });

    // 各モデルが持つ描画ターゲットをテクスチャとする場合
    if (_renderTarget == SelectTarget_ModelFrameBuffer && _renderSprite)
    {
        const GLfloat uvVertex[] =
        {
            1.0f, 1.0f,
            0.0f, 1.0f,
            0.0f, 0.0f,
            1.0f, 0.0f,
        };

        for(csmUint32 i=0; i<_manager.GetModelNum(); i++)
        {
            LAppModel* model = _manager.GetModel(i);
            float alpha = i < 1 ? 1.0f : model->GetOpacity(); // 片方のみ不透明度を取得できるようにする
            _renderSprite->SetColor(1.0f * alpha, 1.0f * alpha, 1.0f * alpha, alpha);

            if (model)
            {
                _renderSprite->RenderImmidiate(model->GetRenderBuffer().GetColorBuffer(), uvVertex, _width, _height);
            }
        }
    }
}

void LAppView::PresentOpacityFrame(float opacity)
{
    _opacityFrame.Present(opacity, [this](GLuint texture, float alpha) {
        const GLfloat uv[] = {1, 1, 0, 1, 0, 0, 1, 0};
        glUseProgram(_spriteShader->GetShaderId());
        glActiveTexture(GL_TEXTURE0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        // Both RGB and alpha must be scaled for premultiplied-alpha blending.
        _renderSprite->SetColor(alpha, alpha, alpha, alpha);
        _renderSprite->RenderImmidiate(texture, uv, _width, _height);
        _renderSprite->SetColor(1, 1, 1, 1);
    });
}

void LAppView::InitializeSprite()
{
    GLuint programId = _spriteShader->GetShaderId();

    int width = _width, height = _height;

    // 画面全体を覆うサイズ
    float x = width * 0.5f;
    float y = height * 0.5f;
    _renderSprite = new LAppSprite(x, y, static_cast<float>(width), static_cast<float>(height), 0, programId);

}

void LAppView::OnTouchesBegan(float px, float py) const
{
    _touchManager->TouchesBegan(px, py);
}

void LAppView::OnTouchesMoved(float px, float py) const
{
    float viewX = this->TransformViewX(_touchManager->GetX());
    float viewY = this->TransformViewY(_touchManager->GetY());

    _touchManager->TouchesMoved(px, py);

    _manager.OnDrag(viewX, viewY);
}

void LAppView::OnTouchesEnded(float px, float py, ModelTransform transform) const
{
    // タッチ終了
    _manager.OnDrag(0.0f, 0.0f);
    {

        // シングルタップ
        if (_width <= 0 || _height <= 0) return;
        const float x = 2.0f * px / _width - 1.0f;
        const float y = 1.0f - 2.0f * py / _height;
        if (DebugTouchLogEnable)
        {
            LAppPal::PrintLogLn("[APP]touchesEnded x:%.2f y:%.2f", x, y);
        }
        _manager.SetViewMatrix(_viewMatrix);
        _manager.OnTap(x, y, _width, _height, transform);
    }
}

void LAppView::PreModelDraw(LAppModel &refModel)
{
    // 別のレンダリングターゲットへ向けて描画する場合の使用するフレームバッファ
    Csm::Rendering::CubismRenderTarget_OpenGLES2* useTarget = NULL;

    if (_renderTarget != SelectTarget_None)
    {// 別のレンダリングターゲットへ向けて描画する場合

        // 透過設定
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        // 使用するターゲット
        useTarget = (_renderTarget == SelectTarget_ViewFrameBuffer) ? &_renderBuffer : &refModel.GetRenderBuffer();

        if (!useTarget->IsValid())
        {// 描画ターゲット内部未作成の場合はここで作成
            int bufWidth = _width, bufHeight = _height;

            if(bufWidth!=0 && bufHeight!=0)
            {
                // モデル描画キャンバス
                useTarget->CreateRenderTarget(static_cast<csmUint32>(bufWidth), static_cast<csmUint32>(bufHeight));
            }
        }

        // レンダリング開始
        useTarget->BeginDraw();
        useTarget->Clear(_clearColor[0], _clearColor[1], _clearColor[2], _clearColor[3]); // 背景クリアカラー
    }
}

void LAppView::PostModelDraw(LAppModel &refModel)
{
    // 別のレンダリングターゲットへ向けて描画する場合の使用するフレームバッファ
    Csm::Rendering::CubismRenderTarget_OpenGLES2* useTarget = NULL;

    if (_renderTarget != SelectTarget_None)
    {// 別のレンダリングターゲットへ向けて描画する場合

        // 使用するターゲット
        useTarget = (_renderTarget == SelectTarget_ViewFrameBuffer) ? &_renderBuffer : &refModel.GetRenderBuffer();

        // レンダリング終了
        useTarget->EndDraw();

        // LAppViewの持つフレームバッファを使うなら、スプライトへの描画はここ
        if (_renderTarget == SelectTarget_ViewFrameBuffer && _renderSprite)
        {
            const GLfloat uvVertex[] =
            {
                1.0f, 1.0f,
                0.0f, 1.0f,
                0.0f, 0.0f,
                1.0f, 0.0f,
            };

            _renderSprite->SetColor(1.0f * GetSpriteAlpha(0), 1.0f * GetSpriteAlpha(0), 1.0f * GetSpriteAlpha(0), GetSpriteAlpha(0));
            _renderSprite->RenderImmidiate(useTarget->GetColorBuffer(), uvVertex, _width, _height);
        }
    }
}

void LAppView::SwitchRenderingTarget(SelectTarget targetType)
{
    _renderTarget = targetType;
}

void LAppView::SetRenderTargetClearColor(float r, float g, float b)
{
    _clearColor[0] = r;
    _clearColor[1] = g;
    _clearColor[2] = b;
}


float LAppView::GetSpriteAlpha(int assign) const
{
    // assignの数値に応じて適当に決定
    float alpha = 0.4f + static_cast<float>(assign) * 0.5f; // サンプルとしてαに適当な差をつける
    if (alpha > 1.0f)
    {
        alpha = 1.0f;
    }
    if (alpha < 0.1f)
    {
        alpha = 0.1f;
    }

    return alpha;
}

void LAppView::ResizeSprite()
{
    LAppTextureManager* textureManager = &_textures;
    if (!textureManager)
    {
        return;
    }

    // 描画領域サイズ
    int width = _width, height = _height;

    float x = 0.0f;
    float y = 0.0f;
    float fWidth = 0.0f;
    float fHeight = 0.0f;

    if (_back)
    {
        GLuint id = _back->GetTextureId();
        LAppTextureManager::TextureInfo* texInfo = textureManager->GetTextureInfoById(id);
        if (texInfo)
        {
            x = width * 0.5f;
            y = height * 0.5f;
            fHeight = static_cast<float>(height) * 0.95f;
            float ratio = fHeight / static_cast<float>(texInfo->height);
            fWidth = static_cast<float>(texInfo->width) * ratio;
            _back->ResetRect(x, y, fWidth, fHeight);
        }
    }

    if (_power)
    {
        GLuint id = _power->GetTextureId();
        LAppTextureManager::TextureInfo* texInfo = textureManager->GetTextureInfoById(id);
        if (texInfo)
        {
            x = static_cast<float>(width - texInfo->width * 0.5f);
            y = static_cast<float>(texInfo->height * 0.5f);
            fWidth = static_cast<float>(texInfo->width);
            fHeight = static_cast<float>(texInfo->height);
            _power->ResetRect(x, y, fWidth, fHeight);
        }
    }

    if (_gear)
    {
        GLuint id = _gear->GetTextureId();
        LAppTextureManager::TextureInfo* texInfo = textureManager->GetTextureInfoById(id);
        if (texInfo)
        {
            x = static_cast<float>(width - texInfo->width * 0.5f);
            y = static_cast<float>(height - texInfo->height * 0.5f);
            fWidth = static_cast<float>(texInfo->width);
            fHeight = static_cast<float>(texInfo->height);
            _gear->ResetRect(x, y, fWidth, fHeight);
        }
    }

    if (_renderSprite)
    {
        x = width * 0.5f;
        y = height * 0.5f;
        _renderSprite->ResetRect(x, y, static_cast<float>(width), static_cast<float>(height));
    }
}

void LAppView::DestroySpriteRenderTarget()
{
    if (_renderTarget == SelectTarget_ViewFrameBuffer)
    {
        _renderBuffer.DestroyRenderTarget();
    }
    else if (_renderTarget == SelectTarget_ModelFrameBuffer)
    {
        for (csmUint32 i = 0; i < _manager.GetModelNum(); i++)
        {
            LAppModel* model = _manager.GetModel(i);
            if (model)
            {
                model->GetRenderBuffer().DestroyRenderTarget();
            }
        }
    }
}
