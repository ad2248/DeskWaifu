/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#include "LAppDefine.hpp"
#include <CubismFramework.hpp>

namespace LAppDefine {

    using namespace Csm;

    // 视图。
    const csmFloat32 ViewScale = 1.0f;
    const csmFloat32 ViewMaxScale = 2.0f;
    const csmFloat32 ViewMinScale = 0.8f;

    const csmFloat32 ViewLogicalLeft = -1.0f;
    const csmFloat32 ViewLogicalRight = 1.0f;
    const csmFloat32 ViewLogicalBottom = -1.0f;
    const csmFloat32 ViewLogicalTop = 1.0f;

    const csmFloat32 ViewLogicalMaxLeft = -2.0f;
    const csmFloat32 ViewLogicalMaxRight = 2.0f;
    const csmFloat32 ViewLogicalMaxBottom = -2.0f;
    const csmFloat32 ViewLogicalMaxTop = 2.0f;

    std::string ModelsDir = "";
    // 相对路径。
    const csmChar* ResourcesPath = "Resources/";

    // 模型后方的背景图片文件。
    const csmChar* BackImageName = "back_class_normal.png";
    // 齿轮图标。
    const csmChar* GearImageName = "icon_gear.png";
    // 退出按钮。
    const csmChar* PowerImageName = "close.png";

    // 着色器相对路径。
    const csmChar* ShaderPath = "SampleShaders/";
    // 顶点着色器。
    const csmChar* VertShaderName = "VertSprite.vert";
    // 片元着色器。
    const csmChar* FragShaderName = "FragSprite.frag";

    // 模型定义------------------------------------------
    // 与外部 JSON 定义文件保持一致。
    const csmChar* MotionGroupIdle = "Idle"; // 待机。
    const csmChar* MotionGroupTapBody = "TapBody"; // 点击身体。

    // 与外部 JSON 定义文件保持一致。
    const csmChar* HitAreaNameHead = "Head";
    const csmChar* HitAreaNameBody = "Body";

    // 动作优先级常量。
    const csmInt32 PriorityNone = 0;
    const csmInt32 PriorityIdle = 1;
    const csmInt32 PriorityNormal = 2;
    const csmInt32 PriorityForce = 3;

    // 调试日志显示选项。
    const csmBool DebugLogEnable = true;
    const csmBool DebugTouchLogEnable = false;

    // 框架输出日志的级别设置。
    const CubismFramework::Option::LogLevel CubismLoggingLevel = CubismFramework::Option::LogLevel_Verbose;

    // 默认渲染目标尺寸。
    csmInt32 RenderTargetWidth = 1900;
    csmInt32 RenderTargetHeight = 1000;
}
