/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#pragma once

#include <string>
#include <GL/glew.h>
#include "LAppWayland.hpp"
#include "InputController.hpp"
#include "ApplicationService.hpp"
#include "Type/csmVector.hpp"
#include "LAppAllocator_Common.hpp"

class LAppView;
class LAppTextureManager;
class LAppLive2DManager;
class LAppIPC;
class AppCommandHandler;

/**
* @brief   アプリケーションクラス。
*   Cubism3の管理を行う。
*/
class LAppDelegate : private IApplicationRuntime
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

    /**
    * @brief   OpenGL用 glfwSetMouseButtonCallback用関数。
    *
    * @param[in]       window            コールバックを呼んだWindow情報
    * @param[in]       button            ボタン種類
    * @param[in]       action            実行結果
    * @param[in]       modify
    */
    void OnMouseCallBack(void* window, int button, int action, int modify);

    /**
    * @brief   OpenGL用 glfwSetCursorPosCallback用関数。
    *
    * @param[in]       window            コールバックを呼んだWindow情報
    * @param[in]       x                 x座標
    * @param[in]       y                 x座標
    */
    void OnMouseCallBack(void* window, double x, double y);

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
    * @brief   WaylandContextを取得する。
    */
    WaylandContext* GetWindow() { return &_wlContext; }

    /**
    * @brief   View情報を取得する。
    */
    LAppView* GetView() { return _view; }
    ApplicationService& Service() { return _service; }
    const ApplicationService& Service() const { return _service; }

    /**
    * @brief   アプリケーションを終了するかどうか。
    */
    bool GetIsEnd() { return _isEnd; }

    /**
    * @brief   アプリケーションを終了する。
    */
    void AppEnd() { _isEnd = true; }

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

    LAppAllocator_Common _cubismAllocator;              ///< Cubism3 Allocator
    Csm::CubismFramework::Option _cubismOption;         ///< Cubism3 Option
    WaylandContext _wlContext;                          ///< Wayland Context
    LAppView* _view;                                    ///< View情報
    LAppLive2DManager* _manager;
    LAppIPC* _ipc;
    AppCommandHandler* _commands;
    ApplicationService _service;
    float _mouseX;                               ///< マウスX座標
    float _mouseY;                               ///< マウスY座標
    InputController _input;
    void FlushPendingTap();
    bool _isEnd;                                 ///< APP終了しているか
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
    void OnScrollCallBack(void* window, double xoffset, double yoffset);

};

class EventHandler
{
public:
    /**
    * @brief   glfwSetMouseButtonCallback用コールバック関数。
    */
    static void OnMouseCallBack(void* window, int button, int action, int modify)
    {
        LAppDelegate::GetInstance()->OnMouseCallBack(window, button, action, modify);
    }

    /**
    * @brief   glfwSetCursorPosCallback用コールバック関数。
    */
    static void OnMouseCallBack(void* window, double x, double y)
    {
         LAppDelegate::GetInstance()->OnMouseCallBack(window, x, y);
    }
    
    /**
    * @brief   glfwSetScrollCallback用コールバック関数。
    */
    static void OnScrollCallBack(void* window, double xoffset, double yoffset)
    {
         LAppDelegate::GetInstance()->OnScrollCallBack(window, xoffset, yoffset);
    }
};
