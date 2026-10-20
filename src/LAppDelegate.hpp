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
#include "InputController.hpp"
#include "CursorLook.hpp"
#include "KWinCursorTracker.hpp"
#include "HoverOpacity.hpp"
#include "HoverOpacitySettings.hpp"
#include "ApplicationService.hpp"
#include "TransformEditor.hpp"
#include "ScreenCropInteraction.hpp"
#include "ModelWindowState.hpp"
#include "OutputSettings.hpp"
#include <memory>
#include "CompositorAdapter.hpp"
#include "Type/csmVector.hpp"
#include "LAppAllocator_Common.hpp"

class LAppView;
class LAppTextureManager;
class LAppLive2DManager;
class LAppIPC;
class AppCommandHandler;
class MiniMaxSpeechSource;
class AudioPlayer;
class SpeechPlayer;

/**
* @brief   アプリケーションクラス。
*   Cubism3の管理を行う。
*/
class LAppDelegate : private IApplicationRuntime, public IDesktopWindowEvents
{
public:
    /**
    * @brief   クラスのインスタンス（シングルトン）を返す。<br>
    *           インスタンスが生成されていない場合は内部でインスタンを生成する。
    *
    * @return  クラスのインスタンス
    */
    static LAppDelegate* GetInstance();

    /**
    * @brief   クラスのインスタンス（シングルトン）を解放する。
    *
    */
    static void ReleaseInstance();

    /**
    * @brief   APPに必要なものを初期化する。
    */
    bool Initialize();

    /**
    * @brief   解放する。
    */
    bool Release();

    /**
    * @brief   実行処理。
    */
    bool Run();

    void SetMousePosition(double x, double y);
    void OnModelKey(uint32_t code, bool pressed);
    /** @brief Reset pressed keys on keyboard enter/leave, seat capability loss,
     *         and explicit keyboard-grab release.
     */
    void ResetKeyboardState();

    /**
     * @brief   ウィンドウクライアント領域の幅、高さ取得
     */
    static void GetClientSize(int& rWidth, int& rHeight);

    /**
    * @brief   デスクトップウィンドウを取得する。
    */
    DesktopWindow* GetWindow() { return _window.get(); }

    /**
    * @brief   View情報を取得する。
    */
    LAppView* GetView() { return _view; }
    ApplicationService& Service() { return _service; }
    const ApplicationService& Service() const { return _service; }
    TransformEditor& Settings() { return *_settings; }
    HoverOpacitySettings& HoverSettings() { return *_hoverSettings; }
    std::vector<OutputSnapshot> Outputs() const { return _window->Outputs(); }
    OutputId CurrentOutput() const { return _window->CurrentOutput(); }
    AppResult SelectOutput(const std::string& name)
    {
        OutputSettings settings(*_window, *_settings, LAppConfig::GetInstance());
        return settings.Select(name);
    }

    /**
    * @brief   アプリケーションを終了するかどうか。
    */
    bool GetIsEnd() { return _windowState.Current() == ModelWindowState::State::Closing ||
        _windowState.Current() == ModelWindowState::State::Closed; }
    ModelWindowState::State WindowState() const { return _windowState.Current(); }

    /**
    * @brief   アプリケーションを終了する。
    */
    void AppEnd() { _windowState.Send(ModelWindowState::Event::Close); }

    /**
     * @brief   アプリケーションの実行パスを設定する。
     */
    void SetExecuteAbsolutePath();

    /**
     * @brief   アプリケーションの実行パスを取得する。
     */
    std::string GetExecuteAbsolutePath(){ return _executeAbsolutePath;}

    /**
     * @brief   テクスチャマネージャーを取得する。
     */
    LAppTextureManager* GetTextureManager() { return _textureManager; }

    /**
     * @brief ウインドウ幅の取得
     *
     * @return ウインドウ幅を返す
     */
    int GetWindowWidth() { return _windowWidth; }

    /**
     * @brief ウインドウ高さの取得
     *
     * @return ウインドウ高さを返す
     */
    int GetWindowHeight() { return _windowHeight; }

private:
    void PointerEntered(double x, double y) override { _hoverOpacity.Enter(x, y); SetMousePosition(x, y); }
    void PointerLeft() override { _hoverOpacity.Leave(); }
    void PointerMoved(double x, double y) override;
    void PointerButton(int button, bool pressed) override;
    void PointerScroll(double vertical) override;
    void KeyboardChanged() override { ResetKeyboardState(); }
    void KeyboardKey(std::uint32_t code, bool pressed) override { OnModelKey(code, pressed); }
    void SurfaceClosed() override { AppEnd(); }
    void OutputSwitchStarted() override;
    void OutputSwitchFinished() override;
    bool SelectModel(int index) override;
    int ModelIndex() const override;
    int ModelCount() const override;
    bool SetHidden(bool hidden) override;
    bool PlayMotion(const std::string& group, int index, int priority) override;
    bool SetExpression(const std::string& id) override;
    bool TriggerHotkey(const std::string& id, std::string& error) override;
    bool SwitchSkin() override;
    /**
     * @brief   コンストラクタ
     */
    LAppDelegate();

    /**
     * @brief   デストラクタ
     */
    ~LAppDelegate();

    /**
     * @brief   Cubism3の初期化
     */
    void InitializeCubism();
    bool InitializeRuntime();
    void SyncInteractionMode();
    void CancelInteraction();
    void StartCropGesture();

    LAppAllocator_Common _cubismAllocator;              ///< Cubism3 Allocator
    Csm::CubismFramework::Option _cubismOption;         ///< Cubism3 Option
    std::unique_ptr<DesktopWindow> _window;
    OutputController _outputController;
    BoundedCompositorTransport _compositorTransport;
    CompositorAdapter _compositor;
    LAppView* _view;                                    ///< View情報
    LAppLive2DManager* _manager;
    LAppIPC* _ipc;
    AppCommandHandler* _commands;
    ApplicationService _service;
    std::unique_ptr<TransformEditor> _settings;
    std::unique_ptr<HoverOpacitySettings> _hoverSettings;
    ScreenCropInteraction _cropInteraction;
    OutputId _lastOutput = 0;
    float _mouseX;                               ///< マウスX座標
    float _mouseY;                               ///< マウスY座標
    InputController _input;
    void FlushPendingTap();
    void ResizeView(int width, int height);
    void MoveScreenDrag();
    void TrackGlobalCursor();
    void UpdateCursorLook(ModelTransform transform);
    CursorLook _cursorLook;
    KWinCursorTracker _kwinCursor;
    HoverOpacity _hoverOpacity;
    std::vector<unsigned char> _inputPixels;
    ModelWindowState _windowState;
    std::unique_ptr<MiniMaxSpeechSource> _speechSource;
    std::unique_ptr<AudioPlayer> _audioPlayer;
    std::unique_ptr<SpeechPlayer> _speechPlayer;
    bool _cubismStarted = false;
    LAppTextureManager* _textureManager;         ///< テクスチャマネージャー
    std::string _executeAbsolutePath;            ///< アプリケーションの実行パス

    int _windowWidth;                            ///< Initialize関数で設定したウィンドウ幅
    int _windowHeight;                           ///< Initialize関数で設定したウィンドウ高さ

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
