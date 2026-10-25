/**
 * @brief 供外部程序控制 DeskWaifu 的 IPC 服务端。
 *
 * 监听 Unix 域套接字，接收以换行分隔的 JSON 命令。
 * 套接字路径：$XDG_RUNTIME_DIR/deskwaifu.sock（回退路径：/tmp/deskwaifu.sock）
 */

#pragma once

#include <cstddef>
#include <string>
#include <sys/types.h>
#include <vector>
#include <memory>
#include "contracts/ICommandService.hpp"

class LAppIPC
{
public:
    LAppIPC();
    ~LAppIPC();
    LAppIPC(const LAppIPC&) = delete;
    LAppIPC& operator=(const LAppIPC&) = delete;
    void SetCommandService(ICommandService& commands) { _commands = &commands; }

    /**
     * @brief 初始化 IPC 套接字服务端。
     * @return 成功时返回 true。
     */
    bool Initialize();

    /**
     * @brief 检查新连接并处理命令。
     *        在套接字就绪或存在缓冲工作时调用，不依赖渲染。
     */
    void Poll();
    std::vector<int> ReadDescriptors() const;
    std::vector<int> WriteDescriptors() const;
    bool HasBufferedWork() const;
    void SetReadyCallback(std::function<void()> ready) { *_ready = std::move(ready); }

    /**
     * @brief 清理套接字。
     */
    void Release();

    /**
     * @brief 获取当前使用的套接字路径。
     */
    const std::string& GetSocketPath() const { return _socketPath; }

private:
    // 固定的传输层限制，不影响命令语义。超大帧只关闭对应连接；
    // 读取缓慢的客户端只暂停自身的命令派发。
    static constexpr std::size_t MaxClients = 16;
    static constexpr std::size_t MaxRequestBytes = 64 * 1024;
    static constexpr std::size_t MaxResponseBytes = 8 * 1024 * 1024;
    static constexpr std::size_t AcceptsPerPoll = 4;
    static constexpr std::size_t CommandsPerPoll = 16;
    static constexpr std::size_t CommandsPerClient = 4;
    static constexpr std::size_t WriteBytesPerClient = 64 * 1024;
    static constexpr std::size_t WriteAttemptsPerClient = 16;

    ICommandService* _commands;

    int _serverFd;
    int _socketLockFd = -1;
    std::string _socketPath;
    dev_t _socketDevice = 0;
    ino_t _socketInode = 0;
    bool _ownsSocketPath = false;
    // 每个连接只保留一个待发送回复，不使用无界回复队列。
    struct PendingReply { bool ready = false; std::string response; };
    std::shared_ptr<std::function<void()>> _ready = std::make_shared<std::function<void()>>();
    struct ClientBuffer {
        int fd;
        std::string buf;
        std::string source;
        std::string response;
        std::size_t sent = 0;
        bool eof = false;
        std::shared_ptr<PendingReply> pending;
    };
    std::vector<ClientBuffer> _clients;

    bool FlushClient(ClientBuffer& client, std::size_t& bytes, std::size_t& attempts);
    void CloseClient(const ClientBuffer& client);
    bool AcquireSocketLock();
    void ReleaseSocketLock();

    unsigned long long _nextConnection = 0;
};
