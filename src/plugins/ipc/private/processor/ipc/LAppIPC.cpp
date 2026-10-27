#include "LAppIPC.hpp"
#include "IpcSocketPath.hpp"

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
#include <utility>

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

bool RemoveStaleSocket(const std::string& path, const sockaddr_un& address)
{
    struct stat before{};
    if (lstat(path.c_str(), &before) != 0 || !S_ISSOCK(before.st_mode)) return false;
    if (ProbeEndpoint(address) != EndpointState::Stale) return false;

    struct stat current{};
    if (lstat(path.c_str(), &current) != 0 || current.st_dev != before.st_dev ||
        current.st_ino != before.st_ino || !S_ISSOCK(current.st_mode)) return false;
    return unlink(path.c_str()) == 0;
}

bool EnsurePrivateSocketDirectory(const std::string& path)
{
    if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) return false;
    struct stat directory{};
    return lstat(path.c_str(), &directory) == 0 && S_ISDIR(directory.st_mode) &&
        directory.st_uid == getuid() && (directory.st_mode & 077) == 0;
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
    const char* runtimeDir = getenv("XDG_RUNTIME_DIR");
    _socketPath = IpcSocketPathForUid(runtimeDir, getuid());
    if (!runtimeDir || !*runtimeDir) {
        if (!EnsurePrivateSocketDirectory(IpcSocketDirectoryForUid(getuid()))) {
            fprintf(stderr, "[IPC] Failed to prepare private fallback runtime directory: %s\n", strerror(errno));
            _socketPath.clear();
            return false;
        }
    }
    if (!AcquireSocketLock()) {
        fprintf(stderr, "[IPC] Socket endpoint is being initialized by another instance\n");
        _socketPath.clear();
        return false;
    }

    _serverFd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (_serverFd < 0)
    {
        fprintf(stderr, "[IPC] Failed to create socket: %s\n", strerror(errno));
        ReleaseSocketLock();
        _socketPath.clear();
        return false;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, _socketPath.c_str(), sizeof(addr.sun_path) - 1);

    bool bound = bind(_serverFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    if (!bound)
    {
        if (errno == EADDRINUSE)
        {
            if (RemoveStaleSocket(_socketPath, addr) &&
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

std::vector<int> LAppIPC::ReadDescriptors() const
{
    std::vector<int> descriptors;
    if (_serverFd >= 0) descriptors.push_back(_serverFd);
    for (const auto& client : _clients)
        if (!client.pending && client.response.empty() && !client.eof) descriptors.push_back(client.fd);
    return descriptors;
}

std::vector<int> LAppIPC::WriteDescriptors() const
{
    std::vector<int> descriptors;
    for (const auto& client : _clients)
        if (!client.response.empty()) descriptors.push_back(client.fd);
    return descriptors;
}

bool LAppIPC::HasBufferedWork() const
{
    for (const auto& client : _clients)
        if ((client.pending && client.pending->ready) || (!client.pending && client.response.empty() &&
            (client.eof || client.buf.find('\n') != std::string::npos))) return true;
    return false;
}

void LAppIPC::Release()
{
    *_ready = {};
    for (const auto& client : _clients) CloseClient(client);
    _clients.clear();

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
    _socketLockFd = open((_socketPath + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
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

void LAppIPC::CloseClient(const ClientBuffer& client)
{
    if (_commands) _commands->Disconnected(client.source);
    close(client.fd);
}

bool LAppIPC::FlushClient(ClientBuffer& client, std::size_t& bytes, std::size_t& attempts)
{
    while (client.sent < client.response.size() && bytes && attempts)
    {
        --attempts;
        const ssize_t n = send(client.fd, client.response.data() + client.sent,
            std::min(bytes, client.response.size() - client.sent), MSG_NOSIGNAL);
        if (n > 0) {
            client.sent += static_cast<std::size_t>(n);
            bytes -= static_cast<std::size_t>(n);
        } else if (n < 0 && errno == EINTR) continue;
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        else return false;
    }
    if (client.sent == client.response.size()) {
        // 同时释放大块内存和逻辑上的待发送字节。
        std::string{}.swap(client.response);
        client.sent = 0;
    }
    return true;
}

void LAppIPC::Poll()
{
    if (_serverFd < 0) return;

    // 尝试次数和成功次数均计入预算，避免 EINTR 绕过限制。
    for (std::size_t i = 0; i < AcceptsPerPoll; ++i)
    {
        const int fd = accept4(_serverFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (_clients.size() == MaxClients) { close(fd); continue; }
        ClientBuffer client;
        client.fd = fd;
        client.source = "stream:" + std::to_string(++_nextConnection);
        _clients.push_back(std::move(client));
    }

    // 轮换首先服务的连接，避免持续流水输入时，
    // 全局命令预算始终优先分配给最前面的客户端。
    if (!_clients.empty()) std::rotate(_clients.begin(), _clients.begin() + 1, _clients.end());
    std::size_t commands = 0;
    for (auto it = _clients.begin(); it != _clients.end();)
    {
        auto& client = *it;
        if (client.pending && client.pending->ready) {
            client.response = std::move(client.pending->response);
            client.pending.reset();
            if (client.response.size() > MaxResponseBytes) {
                CloseClient(client); it = _clients.erase(it); continue;
            }
            client.response += '\n';
        }
        std::size_t bytes = WriteBytesPerClient, attempts = WriteAttemptsPerClient;
        bool alive = FlushClient(client, bytes, attempts);
        // 回压：当前客户端的回复尚未发完时，不再执行命令或继续读取。
        // 其他客户端仍可正常推进。
        if (alive && !client.pending && client.response.empty()) {
            char buffer[4096];
            const auto capacity = std::min(sizeof(buffer), MaxRequestBytes + 1 - client.buf.size());
            if (!client.eof && capacity) {
                const ssize_t n = read(client.fd, buffer, capacity);
                if (n > 0) client.buf.append(buffer, static_cast<std::size_t>(n));
                else if (n == 0) client.eof = true;
                else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) alive = false;
            }

            for (std::size_t lines = 0; alive && !client.pending && client.response.empty() &&
                lines < CommandsPerClient && commands < CommandsPerPoll; ++lines) {
                const auto pos = client.buf.find('\n');
                if (pos == std::string::npos) break;
                if (pos > MaxRequestBytes) { alive = false; break; }
                const std::string line = client.buf.substr(0, pos);
                client.buf.erase(0, pos + 1);
                ++commands; // 空行也消耗工作预算。
                if (line.empty()) continue;
                client.pending = std::make_shared<PendingReply>();
                std::weak_ptr<PendingReply> pending = client.pending;
                std::weak_ptr<std::function<void()>> ready = _ready;
                _commands->ExecuteAsync(line, client.source, [pending, ready](std::string response) {
                    if (auto reply = pending.lock()) {
                        if (reply->ready) return;
                        reply->response = std::move(response); reply->ready = true;
                        auto notify = ready.lock();
                        if (notify && *notify) (*notify)();
                    }
                });
                if (!client.pending->ready) break;
                client.response = std::move(client.pending->response);
                client.pending.reset();
                if (client.response.size() > MaxResponseBytes) { alive = false; break; }
                client.response += '\n';
                alive = FlushClient(client, bytes, attempts);
            }
            const auto newline = client.buf.find('\n');
            if (newline > MaxRequestBytes && client.buf.size() > MaxRequestBytes) alive = false;
            // 对端可能半关闭写端，同时继续读取回复。
            if (client.eof && !client.pending && client.response.empty() && newline == std::string::npos) alive = false;
        }
        if (!alive) {
            CloseClient(client);
            it = _clients.erase(it);
        } else ++it;
    }
}
