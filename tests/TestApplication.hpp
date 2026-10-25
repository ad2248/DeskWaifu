#pragma once
#include "app/LAppDelegate.hpp"
#include "processor/interaction/InteractionProcessor.hpp"
#include "ModelProcessorTestHelpers.hpp"

inline ModelProcessor& ProcessorTestAccess::Models(LAppDelegate& app) { return *app._models; }
inline InteractionContext& ProcessorTestAccess::Context(LAppDelegate& app) { return *app._interaction->_context; }

// 旧场景的测试夹具：生产组装入口没有服务定位/领域访问 API。
// 这里只用友元准备和核验 GUI 内部状态；模型动作仍走正式事件队列。
class TestApplication
{
public:
    static TestApplication* GetInstance() { static TestApplication fixture; return &fixture; }
    static void ReleaseInstance() { LAppDelegate::ReleaseInstance(); }
    bool Initialize() { _app = LAppDelegate::GetInstance(); return _app->Initialize(); }
    bool Release() { return _app->Release(); }
    bool Run() { return _app->Run(); }
    InteractionContext& Context() { return ProcessorTestAccess::Context(*_app); }
    ModelProcessor& Models() { return ProcessorTestAccess::Models(*_app); }
    ApplicationService& Service() { return Context().service; }
    TransformEditor& Settings() { return Context().settings; }
    DesktopWindow* GetWindow() { return Context().window.get(); }
    int GetWindowWidth() { return GetWindow()->Width(); }
    int GetWindowHeight() { return GetWindow()->Height(); }
    ModelWindowState::State WindowState() { return Context().mode.Current(); }
    void AppEnd() { Context().AppEnd(); }
    bool GetIsEnd() { return WindowState() == ModelWindowState::State::Closing || WindowState() == ModelWindowState::State::Closed; }
private:
    LAppDelegate* _app = nullptr;
};
