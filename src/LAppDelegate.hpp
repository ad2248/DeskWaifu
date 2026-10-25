/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#pragma once

#include <string>
#include <GL/glew.h>
#include "DesktopWindow.hpp"
#include "HoverOpacitySettings.hpp"
#include "ApplicationService.hpp"
#include "TransformEditor.hpp"
#include "ModelWindowState.hpp"
#include "OutputSettings.hpp"
#include <memory>
#include "CompositorAdapter.hpp"

class LAppIPC;
class AppCommandHandler;
class MiniMaxSpeechSource;
class AudioPlayer;
class SpeechPlayer;
class OcrClient;
class ProcessorRuntime;
class IpcProcessor;
class ModelProcessor;
class CommandProcessor;
class SignalProcessor;
class InteractionProcessor;
class InputListener;

/**
* @brief 应用类。
* 管理 Cubism3 应用生命周期。
*/
class LAppDelegate : private IApplicationRuntime, public IDesktopWindowEvents
{
public:
    /**
    * @brief 返回类的单例。<br>
    *        实例尚未创建时，在内部创建。
    *
    * @return 类实例。
    */
    static LAppDelegate* GetInstance();

    /**
    * @brief 释放类的单例。
    *
    */
    static void ReleaseInstance();

    /**
    * @brief 初始化应用所需资源。
    */
    bool Initialize();

    /**
    * @brief 释放资源。
    */
    bool Release();

    /**
    * @brief 执行应用运行流程。
    */
    bool Run();

    void SetMousePosition(double x, double y);
    void OnModelKey(uint32_t code, bool pressed);
    /** @brief 键盘进入或离开、座席能力丢失以及显式释放键盘捕获时，
     *         重置已按下的按键。
     */
    void ResetKeyboardState();

    /**
     * @brief 获取窗口客户区的宽度和高度。
     */
    static void GetClientSize(int& rWidth, int& rHeight);

    /**
    * @brief 获取桌面窗口。
    */
    DesktopWindow* GetWindow() { return _window.get(); }

    // GUI 侧操作代理；模型和 GL 对象仅由工作线程持有。
    ModelProcessor& Models() { return *_models; }
    ApplicationService& Service() { return _service; }
    const ApplicationService& Service() const { return _service; }
    TransformEditor& Settings() { return *_settings; }
    HoverOpacitySettings& HoverSettings() { return *_hoverSettings; }
    OcrClient& Ocr();
    ProcessorRuntime& Processors() { return *_processors; }
    std::vector<OutputSnapshot> Outputs() const { return _window->Outputs(); }
    OutputId CurrentOutput() const { return _window->CurrentOutput(); }
    AppResult SelectOutput(const std::string& name)
    {
        OutputSettings settings(*_window, *_settings, LAppConfig::GetInstance());
        return settings.Select(name);
    }

    /**
    * @brief 查询应用是否正在退出。
    */
    bool GetIsEnd() { return _windowState.Current() == ModelWindowState::State::Closing ||
        _windowState.Current() == ModelWindowState::State::Closed; }
    ModelWindowState::State WindowState() const { return _windowState.Current(); }

    /**
    * @brief 结束应用。
    */
    void AppEnd();

    /**
     * @brief 设置应用运行路径。
     */
    void SetExecuteAbsolutePath();

    /**
     * @brief 获取应用运行路径。
     */
    std::string GetExecuteAbsolutePath(){ return _executeAbsolutePath;}

    /**
     * @brief 获取窗口宽度。
     *
     * @return 窗口宽度。
     */
    int GetWindowWidth() { return _windowWidth; }

    /**
     * @brief 获取窗口高度。
     *
     * @return 窗口高度。
     */
    int GetWindowHeight() { return _windowHeight; }

private:
    void PointerEntered(double x, double y) override;
    void PointerLeft() override;
    void PointerMoved(double x, double y) override;
    void PointerButton(int button, bool pressed) override;
    void PointerScroll(double vertical) override;
    void KeyboardChanged() override { ResetKeyboardState(); }
    void KeyboardKey(std::uint32_t code, bool pressed) override { OnModelKey(code, pressed); }
    void SurfaceClosed() override { AppEnd(); }
    void OutputSwitchStarted() override;
    void OutputSwitchFinished() override;
    int ModelIndex() const override;
    int ModelCount() const override;
    bool SetHidden(bool hidden) override;
    /**
     * @brief 构造函数。
     */
    LAppDelegate();

    /**
     * @brief 析构函数。
     */
    ~LAppDelegate();

    bool InitializeRuntime();
    void SyncInteractionMode();
    void CancelInteraction();

    std::unique_ptr<DesktopWindow> _window;
    OutputController _outputController;
    BoundedCompositorTransport _compositorTransport;
    CompositorAdapter _compositor;
    std::unique_ptr<ModelProcessor> _models;
    LAppIPC* _ipc;
    AppCommandHandler* _commands;
    std::unique_ptr<CommandProcessor> _commandProcessor;
    ApplicationService _service;
    std::unique_ptr<TransformEditor> _settings;
    std::unique_ptr<HoverOpacitySettings> _hoverSettings;
    OutputId _lastOutput = 0;
    void ResizeView(int width, int height);
    void MoveScreenDrag();
    std::unique_ptr<InteractionProcessor> _interaction;
    std::unique_ptr<InputListener> _inputListener;
    ModelWindowState _windowState;
    std::unique_ptr<MiniMaxSpeechSource> _speechSource;
    std::unique_ptr<AudioPlayer> _audioPlayer;
    std::unique_ptr<SpeechPlayer> _speechPlayer;
    std::unique_ptr<ProcessorRuntime> _processors;
    std::unique_ptr<IpcProcessor> _ipcProcessor;
    std::unique_ptr<SignalProcessor> _signals;
    std::string _executeAbsolutePath;            ///< 应用运行路径。

    int _windowWidth;                            ///< Initialize 设置的窗口宽度。
    int _windowHeight;                           ///< Initialize 设置的窗口高度。

    float _lookCenterX;
    float _lookCenterY;
    
public:
public:
    void SetLookCenter(float x, float y) {
        _lookCenterX = x;
        _lookCenterY = y;
    }
    float GetLookCenterX() const { return _lookCenterX; }
    float GetLookCenterY() const { return _lookCenterY; }
};
