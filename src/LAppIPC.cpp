#include "LAppIPC.hpp"

#include <sys/socket.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <poll.h>

namespace {
constexpr int ProbeTimeoutMs = 50;

enum class EndpointState { Active, Stale, Unknown };

EndpointState ProbeEndpoint(const sockaddr_un& address)
{
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return EndpointState::Unknown;

    if (connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
        close(fd);
        return EndpointState::Active;
    }
    const int connectError = errno;
    if (connectError == ECONNREFUSED) {
        close(fd);
        return EndpointState::Stale;
    }
    if (connectError != EINPROGRESS && connectError != EAGAIN) {
        close(fd);
        return EndpointState::Unknown;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ProbeTimeoutMs);
    while (true) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) break;
        pollfd pending{fd, POLLOUT, 0};
        const int ready = poll(&pending, 1, static_cast<int>(remaining));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) break;
        int error = 0;
        socklen_t length = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0) {
            close(fd);
            return error == 0 ? EndpointState::Active :
                (error == ECONNREFUSED ? EndpointState::Stale : EndpointState::Unknown);
        }
        break;
    }
    close(fd);
    return EndpointState::Unknown;
}
}

LAppIPC::LAppIPC()
    : _commands(nullptr)
    , _serverFd(-1)
{
}

LAppIPC::~LAppIPC()
{
    Release();
}

bool LAppIPC::Initialize()
{
    if (!_commands) return false;
    // Determine socket path
    const char* runtimeDir = getenv("XDG_RUNTIME_DIR");
    if (runtimeDir && strlen(runtimeDir) > 0)
    {
        _socketPath = std::string(runtimeDir) + "/deskwaifu.sock";
    }
    else
    {
        _socketPath = "/tmp/deskwaifu.sock";
    }
    if (!AcquireSocketLock()) {
        fprintf(stderr, "[IPC] Socket endpoint is being initialized by another instance\n");
        _socketPath.clear();
        return false;
    }

    _serverFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (_serverFd < 0)
    {
        fprintf(stderr, "[IPC] Failed to create socket: %s\n", strerror(errno));
        ReleaseSocketLock();
        _socketPath.clear();
        return false;
    }

    // Set non-blocking
    int flags = fcntl(_serverFd, F_GETFL, 0);
    fcntl(_serverFd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, _socketPath.c_str(), sizeof(addr.sun_path) - 1);

    bool bound = bind(_serverFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    if (!bound)
    {
        if (errno == EADDRINUSE)
        {
            if (ProbeEndpoint(addr) == EndpointState::Stale && unlink(_socketPath.c_str()) == 0 &&
                bind(_serverFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) bound = true;
        }
    }
    if (!bound)
    {
        fprintf(stderr, "[IPC] Failed to bind socket: %s\n", strerror(errno));
        close(_serverFd);
        _serverFd = -1;
        ReleaseSocketLock();
        _socketPath.clear();
        return false;
    }
    struct stat socketStat{};
    if (lstat(_socketPath.c_str(), &socketStat) != 0)
    {
        fprintf(stderr, "[IPC] Failed to inspect socket: %s\n", strerror(errno));
        close(_serverFd);
        _serverFd = -1;
        unlink(_socketPath.c_str());
        ReleaseSocketLock();
        _socketPath.clear();
        return false;
    }
    _socketDevice = socketStat.st_dev;
    _socketInode = socketStat.st_ino;
    _ownsSocketPath = true;

    if (listen(_serverFd, 4) < 0)
    {
        fprintf(stderr, "[IPC] Failed to listen: %s\n", strerror(errno));
        close(_serverFd);
        _serverFd = -1;
        if (_ownsSocketPath) unlink(_socketPath.c_str());
        _ownsSocketPath = false;
        ReleaseSocketLock();
        _socketPath.clear();
        return false;
    }

    fprintf(stderr, "[IPC] Listening on %s\n", _socketPath.c_str());
    return true;
}

void LAppIPC::Release()
{
    for (size_t i = 0; i < _clients.size(); i++)
    {
        for (const auto& client : _clientBuffers)
            if (client.fd == _clients[i] && _commands) _commands->Disconnected(client.source);
        close(_clients[i]);
    }
    _clients.clear();
    _clientBuffers.clear();

    if (_serverFd >= 0)
    {
        close(_serverFd);
        _serverFd = -1;
    }

    struct stat socketStat{};
    if (_ownsSocketPath && !_socketPath.empty() && lstat(_socketPath.c_str(), &socketStat) == 0 &&
        socketStat.st_dev == _socketDevice && socketStat.st_ino == _socketInode)
    {
        unlink(_socketPath.c_str());
    }
    _ownsSocketPath = false;
    ReleaseSocketLock();
    _socketPath.clear();
}

bool LAppIPC::AcquireSocketLock()
{
    _socketLockFd = open((_socketPath + ".lock").c_str(), O_CREAT | O_RDWR, 0600);
    if (_socketLockFd < 0) return false;
    if (flock(_socketLockFd, LOCK_EX | LOCK_NB) == 0) return true;
    close(_socketLockFd);
    _socketLockFd = -1;
    return false;
}

void LAppIPC::ReleaseSocketLock()
{
    if (_socketLockFd >= 0) close(_socketLockFd);
    _socketLockFd = -1;
}

LAppIPC::ClientBuffer& LAppIPC::GetClientBuffer(int fd)
{
    for (size_t i = 0; i < _clientBuffers.size(); i++)
    {
        if (_clientBuffers[i].fd == fd) return _clientBuffers[i];
    }
    ClientBuffer cb;
    cb.fd = fd;
    cb.source = "stream:" + std::to_string(++_nextConnection);
    _clientBuffers.push_back(cb);
    return _clientBuffers.back();
}

void LAppIPC::RemoveClientBuffer(int fd)
{
    for (size_t i = 0; i < _clientBuffers.size(); i++)
    {
        if (_clientBuffers[i].fd == fd)
        {
            _clientBuffers.erase(_clientBuffers.begin() + i);
            return;
        }
    }
}

void LAppIPC::Poll()
{
    if (_serverFd < 0) return;

    // Accept new connections
    while (true)
    {
        int clientFd = accept(_serverFd, NULL, NULL);
        if (clientFd < 0) break;

        int flags = fcntl(clientFd, F_GETFL, 0);
        fcntl(clientFd, F_SETFL, flags | O_NONBLOCK);
        _clients.push_back(clientFd);
        GetClientBuffer(clientFd);
    }

    // Process existing clients
    std::vector<int> toRemove;
    for (size_t i = 0; i < _clients.size(); i++)
    {
        int fd = _clients[i];
        char buf[4096];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);

        if (n > 0)
        {
            buf[n] = '\0';
            ClientBuffer& client = GetClientBuffer(fd);
            std::string& clientBuf = client.buf;
            clientBuf += buf;

            // Process complete lines
            size_t pos;
            while ((pos = clientBuf.find('\n')) != std::string::npos)
            {
                std::string line = clientBuf.substr(0, pos);
                clientBuf.erase(0, pos + 1);

                if (!line.empty())
                {
                    std::string response = _commands->Execute(line, client.source);
                    response += "\n";
                    // Best-effort write
                    send(fd, response.c_str(), response.size(), MSG_NOSIGNAL);
                }
            }
        }
        else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
        {
            close(fd);
            for (const auto& client : _clientBuffers)
                if (client.fd == fd && _commands) _commands->Disconnected(client.source);
            RemoveClientBuffer(fd);
            toRemove.push_back(fd);
        }
    }

    // Remove disconnected clients
    for (size_t i = 0; i < toRemove.size(); i++)
    {
        _clients.erase(std::remove(_clients.begin(), _clients.end(), toRemove[i]), _clients.end());
    }
}
