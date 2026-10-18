#pragma once

#include <cstdlib>
#include <string>
#include <sys/types.h>
#include <unistd.h>

inline std::string IpcSocketDirectoryForUid(uid_t uid)
{
    return "/tmp/deskwaifu-" + std::to_string(static_cast<unsigned long>(uid));
}

inline std::string IpcSocketPathForUid(const char* runtimeDir, uid_t uid)
{
    if (runtimeDir && *runtimeDir) return std::string(runtimeDir) + "/deskwaifu.sock";
    return IpcSocketDirectoryForUid(uid) + "/deskwaifu.sock";
}

inline std::string IpcSocketPath()
{
    return IpcSocketPathForUid(getenv("XDG_RUNTIME_DIR"), getuid());
}
