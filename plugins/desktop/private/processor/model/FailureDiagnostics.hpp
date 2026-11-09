#pragma once

#include <string>

// 组合包含文件名和失败原因的纹理诊断，不执行日志输出。
inline std::string TextureFailureMessage(const std::string& file, const std::string& reason)
{
    return "[Texture] " + reason + ": " + file;
}

// 包装驱动链接日志；驱动未提供日志时返回明确的失败说明。
inline std::string ShaderLinkFailureMessage(const std::string& driverLog)
{
    return driverLog.empty() ? "Shader program link failed without a driver log"
        : "Shader link log: " + driverLog;
}
