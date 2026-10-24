/**
 * @brief IPC server for controlling DeskWaifu from external programs.
 *
 * Listens on a Unix domain socket and accepts newline-delimited JSON commands.
 * Socket path: $XDG_RUNTIME_DIR/deskwaifu.sock (fallback: /tmp/deskwaifu.sock)
 */

#pragma once

#include <cstddef>
#include <string>
#include <sys/types.h>
#include <vector>
#include <memory>
#include "ICommandService.hpp"

class LAppIPC
{
public:
    LAppIPC();
    ~LAppIPC();
    LAppIPC(const LAppIPC&) = delete;
    LAppIPC& operator=(const LAppIPC&) = delete;
    void SetCommandService(ICommandService& commands) { _commands = &commands; }

    /**
     * @brief Initialize the IPC socket server.
     * @return true on success
     */
    bool Initialize();

    /**
     * @brief Poll for incoming connections and process commands.
     *        Called on socket readiness (or buffered work), independently of rendering.
     */
    void Poll();
    std::vector<int> ReadDescriptors() const;
    std::vector<int> WriteDescriptors() const;
    bool HasBufferedWork() const;
    void SetReadyCallback(std::function<void()> ready) { *_ready = std::move(ready); }

    /**
     * @brief Clean up the socket.
     */
    void Release();

    /**
     * @brief Get the socket path being used.
     */
    const std::string& GetSocketPath() const { return _socketPath; }

private:
    // Fixed transport bounds, not command semantics. Oversized frames close
    // only their connection; a slow reader pauses its own command dispatch.
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
    // One pending reply per connection; no unbounded response queue.
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
