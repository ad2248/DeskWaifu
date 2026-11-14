#pragma once

#include <cstdlib>
#include <string>
#include <sys/types.h>
#include <unistd.h>

// 返回指定用户在缺少运行时目录时使用的私有目录路径；不创建目录。
inline std::string IpcSocketDirectoryForUid(uid_t uid) { return "/tmp/deskwaifu-" + std::to_string(static_cast<unsigned long>(uid)); }

// 优先拼接非空 runtimeDir，否则使用 uid 对应的回退目录。
inline std::string IpcSocketPathForUid(const char* runtimeDir, uid_t uid)
{
    if (runtimeDir && *runtimeDir) {
        return std::string(runtimeDir) + "/deskwaifu.sock";
    }
    return IpcSocketDirectoryForUid(uid) + "/deskwaifu.sock";
}

// 返回当前进程环境和真实用户 ID 对应的套接字路径。
inline std::string IpcSocketPath() { return IpcSocketPathForUid(getenv("XDG_RUNTIME_DIR"), getuid()); }
