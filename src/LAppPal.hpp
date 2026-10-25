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
class LAppPal
{
public:
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
    * @return 帧间隔时间，单位为毫秒。
    *
    */
    static Csm::csmFloat32 GetDeltaTime();

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
