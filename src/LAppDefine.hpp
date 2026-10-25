/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#pragma once

#include <string>
#include <CubismFramework.hpp>

/**
* @brief 应用使用的常量。
*
*/
namespace LAppDefine {

    using namespace Csm;

    extern const csmFloat32 ViewScale;              ///< 缩放比例。
    extern const csmFloat32 ViewMaxScale;           ///< 最大缩放比例。
    extern const csmFloat32 ViewMinScale;           ///< 最小缩放比例。

    extern const csmFloat32 ViewLogicalLeft;        ///< 逻辑视图坐标系左边界。
    extern const csmFloat32 ViewLogicalRight;       ///< 逻辑视图坐标系右边界。
    extern const csmFloat32 ViewLogicalBottom;      ///< 逻辑视图坐标系下边界。
    extern const csmFloat32 ViewLogicalTop;         ///< 逻辑视图坐标系上边界。

    extern const csmFloat32 ViewLogicalMaxLeft;     ///< 逻辑视图坐标系左边界极限。
    extern const csmFloat32 ViewLogicalMaxRight;    ///< 逻辑视图坐标系右边界极限。
    extern const csmFloat32 ViewLogicalMaxBottom;   ///< 逻辑视图坐标系下边界极限。
    extern const csmFloat32 ViewLogicalMaxTop;      ///< 逻辑视图坐标系上边界极限。

    extern std::string ModelsDir;                   ///< 模型目录路径。
    extern const csmChar* ResourcesPath;            ///< 资源路径。
    extern const csmChar* BackImageName;         ///< 背景图片文件。
    extern const csmChar* GearImageName;         ///< 齿轮图片文件。
    extern const csmChar* PowerImageName;        ///< 退出按钮图片文件。

    extern const csmChar* ShaderPath;               ///< 着色器路径。
    extern const csmChar* VertShaderName;           ///< 顶点着色器。
    extern const csmChar* FragShaderName;           ///< 片元着色器。

    // 模型定义--------------------------------------------
                                                    // 与外部 JSON 定义文件保持一致。
    extern const csmChar* MotionGroupIdle;          ///< 待机时播放的动作列表。
    extern const csmChar* MotionGroupTapBody;       ///< 点击身体时播放的动作列表。

                                                    // 与外部 JSON 定义文件保持一致。
    extern const csmChar* HitAreaNameHead;          ///< 命中测试的 Head 标签。
    extern const csmChar* HitAreaNameBody;          ///< 命中测试的 Body 标签。

                                                    // 动作优先级常量。
    extern const csmInt32 PriorityNone;             ///< 动作优先级常量：0。
    extern const csmInt32 PriorityIdle;             ///< 动作优先级常量：1。
    extern const csmInt32 PriorityNormal;           ///< 动作优先级常量：2。
    extern const csmInt32 PriorityForce;            ///< 动作优先级常量：3。

                                                    // 调试日志显示。
    extern const csmBool DebugLogEnable;            ///< 是否显示调试日志。
    extern const csmBool DebugTouchLogEnable;       ///< 是否显示触摸处理调试日志。

    // 框架输出日志的级别设置。
    extern const CubismFramework::Option::LogLevel CubismLoggingLevel;

    // 默认渲染目标尺寸。
    extern csmInt32 RenderTargetWidth;
    extern csmInt32 RenderTargetHeight;
}
