/**
 * @brief IPC server for controlling DeskWaifu from external programs.
 *
 * Listens on a Unix domain socket and accepts newline-delimited JSON commands.
 * Socket path: $XDG_RUNTIME_DIR/deskwaifu.sock (fallback: /tmp/deskwaifu.sock)
 */

#pragma once

#include <string>
#include <sys/types.h>
#include <vector>
#include "AppCommandHandler.hpp"

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
     *        Call this once per frame from the main loop.
     */
    void Poll();

    /**
     * @brief Clean up the socket.
     */
    void Release();

    /**
     * @brief Get the socket path being used.
     */
    const std::string& GetSocketPath() const { return _socketPath; }

private:
    void HandleClient(int clientFd);
    ICommandService* _commands;

    int _serverFd;
    std::string _socketPath;
    dev_t _socketDevice = 0;
    ino_t _socketInode = 0;
    bool _ownsSocketPath = false;
    std::vector<int> _clients;

    // Partial read buffers per client fd
    struct ClientBuffer {
        int fd;
        std::string buf;
        std::string source;
    };
    std::vector<ClientBuffer> _clientBuffers;

    ClientBuffer& GetClientBuffer(int fd);
    void RemoveClientBuffer(int fd);

    unsigned long long _nextConnection = 0;
};
