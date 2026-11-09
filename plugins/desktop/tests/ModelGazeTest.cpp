#include <GL/glew.h>
#include <EGL/egl.h>
#include "processor/model/LAppModel.hpp"
#include "LAppAllocator_Common.hpp"
#include "processor/model/LAppPal.hpp"
#include "processor/interaction/ExternalControlState.hpp"
#include "processor/interaction/CursorLook.hpp"
#include "TestTemporaryDirectory.hpp"
#include <Id/CubismIdManager.hpp>
#include <cmath>
#include <iostream>
#include <thread>
#include <chrono>
#include <fstream>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << "Line " << __LINE__ << ": " << #condition << '\n'; return 1; \
} } while (false)

int main()
{
    const auto display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) return 77;
    const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    CHECK(eglChooseConfig(display, attributes, &config, 1, &count) && count == 1);
    CHECK(eglBindAPI(EGL_OPENGL_API));
    const EGLint size[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    const auto surface = eglCreatePbufferSurface(display, config, size);
    const auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
    CHECK(surface != EGL_NO_SURFACE && context != EGL_NO_CONTEXT);
    CHECK(eglMakeCurrent(display, surface, surface, context));
    glewExperimental = GL_TRUE;
    CHECK(glewInit() == GLEW_OK);

    LAppAllocator_Common allocator;
    Csm::CubismFramework::Option option{};
    option.LoadFileFunction = LAppPal::LoadFileAsBytes;
    option.ReleaseBytesFunction = LAppPal::ReleaseBytes;
    CHECK(Csm::CubismFramework::StartUp(&allocator, &option));
    Csm::CubismFramework::Initialize();
    {
        LAppTextureManager textures;
        LAppModel model(textures, 16, 16, -1);
        // 夹具中不包含动作或表情，避免掩盖视线效果。
        const auto directory = MakeTestTemporaryDirectory("deskwaifu-gaze");
        CHECK(!directory.empty());
        const auto setting = directory + "/gaze.model3.json";
        {
            std::ofstream file(setting);
            file << "{\"Version\":3,\"FileReferences\":{\"Moc\":\""
                 << SDK_RESOURCES "/Haru/Haru.moc3\"},\"Groups\":["
                 << "{\"Target\":\"Parameter\",\"Name\":\"LipSync\",\"Ids\":[\"ParamMouthOpenY\"]}]}";
            CHECK(file.good());
        }
        CHECK(model.LoadAssets("", setting.c_str()));
        CHECK(unlink(setting.c_str()) == 0);
        CHECK(rmdir(directory.c_str()) == 0);
        const auto eyeX = Csm::CubismFramework::GetIdManager()->GetId("ParamEyeBallX");
        const auto eyeY = Csm::CubismFramework::GetIdManager()->GetId("ParamEyeBallY");
        const auto mouth = Csm::CubismFramework::GetIdManager()->GetId("ParamMouthOpenY");
        CHECK(model.GetModel()->GetParameterIndex(eyeX) < model.GetModel()->GetParameterCount());
        CHECK(model.GetModel()->GetParameterIndex(eyeY) < model.GetModel()->GetParameterCount());

        // 与运行时一致，每次更新前都重新应用缓存的鼠标视线。
        // 测试真实调度器和眼球参数，而非仅测试模拟目标。
        const auto frames = [&](float mouseX, float mouseY, const ModelFrameInput& input, float radius = 2.f) {
            CursorLook cursor;
            cursor.LocalPosition(100 + mouseX * 100, 100 - mouseY * 100);
            for (int i = 0; i < 90; ++i) {
                const auto look = cursor.LookAt(0, 0, 200, 200, .5f, .5f, {1, 0, 0}, radius);
                model.SetDragging(look.first, look.second);
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                LAppPal::UpdateTime();
                model.Update(input);
            }
        };
        LAppPal::UpdateTime();
        frames(-0.8f, -0.6f, {});
        CHECK(model.GetModel()->GetParameterValue(eyeX) < -0.4f);
        CHECK(model.GetModel()->GetParameterValue(eyeY) < -0.3f);

        // 分别测试真实标志，包括被忽略字段中的非零值和显式零覆盖值。
        // 不另写一套输入派发逻辑。
        model.GetModel()->SetParameterValue(mouth, .25f);
        model.GetModel()->SetParameterValue(eyeX, 0);
        model.GetModel()->SetParameterValue(eyeY, 0);
        model.GetModel()->SaveParameters();
        frames(-.8f, -.6f, {false, .9f, false, .8f, .6f});
        CHECK(model.GetModel()->GetParameterValue(eyeX) < -.4f);
        CHECK(std::abs(model.GetModel()->GetParameterValue(mouth) - .25f) < .001f);
        frames(.8f, .6f, {true, .65f, false, -.8f, -.6f});
        CHECK(model.GetModel()->GetParameterValue(eyeX) > .4f);
        CHECK(std::abs(model.GetModel()->GetParameterValue(mouth) - .65f) < .001f);
        frames(.8f, .6f, {false, .9f, true, -.8f, -.6f});
        CHECK(model.GetModel()->GetParameterValue(eyeX) < -.4f);
        CHECK(model.GetModel()->GetParameterValue(eyeY) < -.3f);
        CHECK(std::abs(model.GetModel()->GetParameterValue(mouth) - .25f) < .001f);
        frames(-.8f, -.6f, {true, 0, true, 0, 0});
        CHECK(std::abs(model.GetModel()->GetParameterValue(eyeX)) < .01f);
        CHECK(std::abs(model.GetModel()->GetParameterValue(eyeY)) < .01f);
        CHECK(std::abs(model.GetModel()->GetParameterValue(mouth)) < .001f);
        frames(0.8f, 0.6f, {});
        CHECK(model.GetModel()->GetParameterValue(eyeX) > 0.4f);
        CHECK(model.GetModel()->GetParameterValue(eyeY) > 0.3f);

        frames(8.f, 6.f, {}, .25f);
        CHECK(std::abs(model.GetModel()->GetParameterValue(eyeX) - .2f) < .01f);
        CHECK(std::abs(model.GetModel()->GetParameterValue(eyeY) - .15f) < .01f);
        frames(8.f, 6.f, {}, 0);
        CHECK(std::abs(model.GetModel()->GetParameterValue(eyeX)) < .01f);
        CHECK(std::abs(model.GetModel()->GetParameterValue(eyeY)) < .01f);

        ExternalControlState controls;
        CHECK(controls.SetLook("control", ExternalControlState::Lifetime::Persistent, 0.8f, 0.6f));
        CHECK(controls.SetMouth("control", ExternalControlState::Lifetime::Persistent, 0.7f));
        frames(-0.8f, -0.6f, controls.Frame(), .1f);
        CHECK(model.GetModel()->GetParameterValue(eyeX) > 0.4f);
        CHECK(model.GetModel()->GetParameterValue(eyeY) > 0.3f);
        CHECK(std::abs(model.GetModel()->GetParameterValue(mouth) - 0.7f) < 0.001f);

        CHECK(controls.SetLook("tracker", ExternalControlState::Lifetime::Stream, -0.8f, -0.6f));
        frames(0.8f, 0.6f, controls.Frame());
        CHECK(model.GetModel()->GetParameterValue(eyeX) < -0.4f);
        CHECK(model.GetModel()->GetParameterValue(eyeY) < -0.3f);
        controls.Release("tracker");
        frames(-0.8f, -0.6f, controls.Frame());
        CHECK(model.GetModel()->GetParameterValue(eyeX) > 0.4f);
        CHECK(model.GetModel()->GetParameterValue(eyeY) > 0.3f);

        controls.ResetLook("control");
        frames(-0.8f, -0.6f, controls.Frame());
        CHECK(model.GetModel()->GetParameterValue(eyeX) < -0.4f);
        CHECK(model.GetModel()->GetParameterValue(eyeY) < -0.3f);
    }
    Csm::CubismFramework::Dispose();
    Csm::CubismFramework::CleanUp();
    CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
    CHECK(eglDestroyContext(display, context));
    CHECK(eglDestroySurface(display, surface));
    CHECK(eglTerminate(display));
    return 0;
}
