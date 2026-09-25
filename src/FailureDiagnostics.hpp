#pragma once

#include <string>

inline std::string TextureFailureMessage(const std::string& file, const std::string& reason)
{
    return "[Texture] " + reason + ": " + file;
}

inline std::string ShaderLinkFailureMessage(const std::string& driverLog)
{
    return driverLog.empty() ? "Shader program link failed without a driver log"
        : "Shader link log: " + driverLog;
}
