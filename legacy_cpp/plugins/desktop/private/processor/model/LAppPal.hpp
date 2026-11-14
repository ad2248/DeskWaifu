/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#pragma once

#include <CubismFramework.hpp>
#include <cstdlib>
#include <string>

/**
 * @brief 抽象平台相关功能的 Cubism 平台抽象层。
 *
 * 集中文件读取、时间获取等平台相关函数。
 *
 */
// 资源基路径和帧时钟为进程级状态，由渲染线程串行更新。
class LAppPal {
public:
    // 设置相对文件路径使用的前缀，调用方负责末尾路径分隔符。
    static void SetResourceBasePath(const std::string& base);
    /**
     * @brief 将文件读取为字节数据。
     *
     * 将文件读取为字节数据。
     *
     * @param[in]   filePath 待读取文件的路径。
     * @param[out]  outSize 文件大小。
     * @return 字节数据。
     */
    // 优先读取内嵌着色器；失败返回空指针，成功缓冲必须用 ReleaseBytes 释放。
    // outSize 必须非空，仅在读取成功时写入字节数。
    static Csm::csmByte* LoadFileAsBytes(const std::string filePath, Csm::csmSizeInt* outSize);

    /**
     * @brief 释放字节数据。
     *
     * 释放字节数据。
     *
     * @param[in]   byteData 待释放的字节数据。
     */
    static void ReleaseBytes(Csm::csmByte* byteData);

    /**
     * @biref 获取与上一帧的时间差。
     *
     * @return 帧间隔时间，单位为秒。
     *
     */
    static Csm::csmFloat32 GetDeltaTime();

    // 以单调时钟推进帧时间，供本帧的 GetDeltaTime 查询。
    static void UpdateTime();

    /**
     * @brief 输出日志。
     *
     * 输出日志。
     *
     * @param[in]   format 格式字符串。
     * @param[in]   ... 可变字符串参数。
     *
     */
    static void PrintLog(const Csm::csmChar* format, ...);

    /**
     * @brief 输出日志并在末尾换行。
     *
     * 输出日志并在末尾换行。
     *
     * @param[in]   format 格式字符串。
     * @param[in]   ... 可变字符串参数。
     *
     */
    static void PrintLogLn(const Csm::csmChar* format, ...);

    /**
     * @brief 输出消息。
     *
     * 输出消息。
     *
     * @param[in]   message 字符串。
     *
     */
    static void PrintMessage(const Csm::csmChar* message);

    /**
     * @brief 输出消息并在末尾换行。
     *
     * 输出消息并在末尾换行。
     *
     * @param[in]   message 字符串。
     *
     */
    static void PrintMessageLn(const Csm::csmChar* message);

private:
    static std::string s_resourceBasePath;
    static double s_currentFrame;
    static double s_lastFrame;
    static double s_deltaTime;
};
