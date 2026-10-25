#include "LAppIPC.hpp"
#include "TestTemporaryDirectory.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {
struct Faults
{
    bool enabled = false;
    bool smallBuffer = true;
    int sendEintr = 1, sendAgain = 1, readEintr = 1, acceptEintr = 1;
    int shortWrites = 0, wouldBlock = 0, accepts = 0;
    std::size_t written = 0;
    std::vector<int> serverFds;
    bool Server(int fd) const
    {
        return enabled && std::find(serverFds.begin(), serverFds.end(), fd) != serverFds.end();
    }
} faults;

struct InjectFaults
{
    InjectFaults() { faults = Faults{}; faults.enabled = true; }
    ~InjectFaults() { faults.enabled = false; }
};
}

extern "C" ssize_t __real_send(int, const void*, size_t, int);
extern "C" ssize_t __wrap_send(int fd, const void* bytes, size_t count, int flags)
{
    if (faults.Server(fd)) {
        if (faults.sendEintr) { --faults.sendEintr; errno = EINTR; return -1; }
        if (faults.sendAgain) { --faults.sendAgain; errno = EAGAIN; return -1; }
    }
    const auto n = __real_send(fd, bytes, count, flags);
    if (faults.Server(fd)) {
        if (n > 0) faults.written += static_cast<std::size_t>(n);
        if (n > 0 && static_cast<size_t>(n) < count) ++faults.shortWrites;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) ++faults.wouldBlock;
    }
    return n;
}

extern "C" ssize_t __real_read(int, void*, size_t);
extern "C" ssize_t __wrap_read(int fd, void* bytes, size_t count)
{
    if (faults.Server(fd) && faults.readEintr) { --faults.readEintr; errno = EINTR; return -1; }
    return __real_read(fd, bytes, count);
}

extern "C" int __real_accept4(int, sockaddr*, socklen_t*, int);
extern "C" int __wrap_accept4(int fd, sockaddr* address, socklen_t* length, int flags)
{
    if (faults.enabled && faults.acceptEintr) { --faults.acceptEintr; errno = EINTR; return -1; }
    const int accepted = __real_accept4(fd, address, length, flags);
    if (faults.enabled && accepted >= 0) {
        const int smallBuffer = 4096;
        if (faults.smallBuffer &&
            setsockopt(accepted, SOL_SOCKET, SO_SNDBUF, &smallBuffer, sizeof(smallBuffer)) != 0) std::abort();
        faults.serverFds.push_back(accepted);
        ++faults.accepts;
    }
    return accepted;
}

namespace {
constexpr std::size_t RequestLimit = 64 * 1024;
constexpr std::size_t ResponseLimit = 8 * 1024 * 1024;
constexpr int ClientLimit = 16;

struct Commands : ICommandService
{
    std::vector<std::function<void(std::string)>> pending;
    std::vector<std::string> requests;
    std::vector<std::string> disconnected;
    std::string Execute(const std::string& request, const std::string&) override
    {
        requests.push_back(request);
        if (request == "large") return std::string(4 * 1024 * 1024, 'L');
        if (request == "maximum") return std::string(ResponseLimit, 'M');
        if (request == "oversized") return std::string(ResponseLimit + 1, 'X');
        return request;
    }
    void Disconnected(const std::string& source) override { disconnected.push_back(source); }
    void ExecuteAsync(const std::string& request, const std::string& source, std::function<void(std::string)> complete) override
    {
        if (request == "delayed") { requests.push_back(request); pending.push_back(std::move(complete)); }
        else complete(Execute(request, source));
    }
};

struct Fixture
{
    Commands commands;
    LAppIPC server;
    std::vector<int> clients;
    std::string directory = MakeTestTemporaryDirectory("deskwaifu-bounded-ipc");
    bool hadRuntime = getenv("XDG_RUNTIME_DIR") != nullptr;
    std::string previous = hadRuntime ? getenv("XDG_RUNTIME_DIR") : "";
    bool ready = false;

    Fixture()
    {
        if (directory.empty()) return;
        setenv("XDG_RUNTIME_DIR", directory.c_str(), 1);
        server.SetCommandService(commands);
        ready = server.Initialize();
    }
    ~Fixture()
    {
        for (int client : clients) close(client);
        server.Release();
        if (!directory.empty()) {
            unlink((directory + "/deskwaifu.sock.lock").c_str());
            rmdir(directory.c_str());
        }
        if (hadRuntime) setenv("XDG_RUNTIME_DIR", previous.c_str(), 1);
        else unsetenv("XDG_RUNTIME_DIR");
    }
    int Connect()
    {
        if (!ready) return -1;
        const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (fd < 0) return -1;
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, server.GetSocketPath().c_str(), sizeof(address.sun_path) - 1);
        if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            close(fd);
            return -1;
        }
        clients.push_back(fd);
        return fd;
    }
    bool Send(int fd, const std::string& bytes)
    {
        std::size_t offset = 0;
        for (int i = 0; i < 10000 && offset < bytes.size(); ++i) {
            const ssize_t n = send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
            if (n > 0) offset += static_cast<std::size_t>(n);
            else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) return false;
            server.Poll();
        }
        return offset == bytes.size();
    }
    static bool Read(int fd, std::string& bytes)
    {
        char buffer[16384];
        const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n > 0) bytes.append(buffer, static_cast<std::size_t>(n));
        return n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
    }
};

bool SlowReader(bool inject)
{
    InjectFaults guard;
    faults.enabled = inject;
    Fixture f;
    const int slow = f.Connect();
    const int fast = f.Connect();
    if (slow < 0 || fast < 0 || !f.Send(slow, "large\nsecond\n")) return false;
    // 不读取回复，将真实 Unix 套接字填满直到写入会阻塞。
    for (int i = 0; i < 128; ++i) f.server.Poll();
    if (std::find(f.commands.requests.begin(), f.commands.requests.end(), "second") !=
        f.commands.requests.end()) return false;
    if (!f.Send(fast, "fast\n")) return false;
    std::string fastReply;
    for (int i = 0; i < 16 && fastReply.empty(); ++i) {
        f.server.Poll();
        Fixture::Read(fast, fastReply);
    }
    if (fastReply != "fast\n") return false;
    const std::string expected = std::string(4 * 1024 * 1024, 'L') + "\nsecond\n";
    std::string received;
    for (int i = 0; i < 10000 && received.size() < expected.size(); ++i) {
        f.server.Poll();
        if (Fixture::Read(slow, received)) return false;
    }
    return received == expected && (!inject || (faults.sendEintr == 0 && faults.sendAgain == 0 &&
        faults.readEintr == 0 && faults.acceptEintr == 0 && faults.shortWrites > 0 && faults.wouldBlock > 0));
}

bool RequestBoundaries()
{
    Fixture f;
    const int client = f.Connect();
    const std::string request(RequestLimit, 'r');
    if (client < 0 || !f.Send(client, request.substr(0, RequestLimit - 1))) return false;
    if (!f.commands.requests.empty() || !f.Send(client, "r\n")) return false;
    std::string reply;
    for (int i = 0; i < 128 && reply.size() < request.size() + 1; ++i) {
        f.server.Poll();
        if (Fixture::Read(client, reply)) return false;
    }
    return f.commands.requests.size() == 1 && f.commands.requests[0] == request && reply == request + '\n';
}

bool RejectLongRequest(bool terminated)
{
    Fixture f;
    const int client = f.Connect();
    if (client < 0) return false;
    f.Send(client, std::string(RequestLimit + 1, 'x') + (terminated ? "\n" : ""));
    std::string reply;
    bool closed = false;
    for (int i = 0; i < 64 && !closed; ++i) {
        f.server.Poll();
        closed = Fixture::Read(client, reply);
    }
    const auto releases = f.commands.disconnected.size();
    f.server.Release();
    return closed && f.commands.requests.empty() && releases == 1 && f.commands.disconnected.size() == 1;
}

bool RejectLargeResponse()
{
    Fixture f;
    const int client = f.Connect();
    if (client < 0 || !f.Send(client, "oversized\n")) return false;
    std::string reply;
    bool closed = false;
    for (int i = 0; i < 32 && !closed; ++i) {
        f.server.Poll();
        closed = Fixture::Read(client, reply);
    }
    return closed && reply.empty() && f.commands.disconnected.size() == 1;
}

bool ConnectionLimit()
{
    Fixture f;
    for (int i = 0; i < ClientLimit; ++i) {
        if (f.Connect() < 0) return false;
        f.server.Poll();
    }
    const int extra = f.Connect();
    if (extra < 0) return false;
    f.server.Poll();
    std::string reply;
    if (!Fixture::Read(extra, reply)) return false;
    for (int i = 0; i < ClientLimit; ++i) {
        if (!f.Send(f.clients[i], "alive\n")) return false;
        reply.clear();
        for (int j = 0; j < 8 && reply.empty(); ++j) {
            f.server.Poll();
            if (Fixture::Read(f.clients[i], reply)) return false;
        }
        if (reply != "alive\n") return false;
    }
    return true;
}

bool BoundedCommands()
{
    Fixture f;
    const int flood = f.Connect();
    const int fast = f.Connect();
    if (flood < 0 || fast < 0) return false;
    std::string requests;
    for (int i = 0; i < 100; ++i) requests += "x\n";
    if (send(flood, requests.data(), requests.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(requests.size()) ||
        send(fast, "fast\n", 5, MSG_NOSIGNAL) != 5) return false;
    f.server.Poll();
    if (f.commands.requests.size() > 16 ||
        std::count(f.commands.requests.begin(), f.commands.requests.end(), "x") > 4) return false;
    std::string fastReply;
    Fixture::Read(fast, fastReply);
    if (fastReply != "fast\n") return false;
    std::string replies;
    for (int i = 0; i < 128 && replies.size() < requests.size(); ++i) {
        f.server.Poll();
        if (Fixture::Read(flood, replies)) return false;
    }
    return replies == requests && f.commands.requests.size() == 101;
}

bool AcceptBudget()
{
    InjectFaults guard;
    Fixture f;
    for (int i = 0; i < 5; ++i) if (f.Connect() < 0) return false;
    f.server.Poll();
    // 注入的 EINTR 也消耗四次 accept 尝试中的一次。
    if (faults.accepts != 3) return false;
    f.server.Poll();
    return faults.accepts == 5;
}

bool GlobalFairness()
{
    Fixture f;
    for (int i = 0; i < ClientLimit; ++i) {
        if (f.Connect() < 0) return false;
        f.server.Poll();
    }
    std::string requests;
    for (int i = 0; i < 100; ++i) requests += "x\n";
    for (int fd : f.clients)
        if (send(fd, requests.data(), requests.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(requests.size())) return false;
    std::vector<std::string> replies(ClientLimit);
    for (int i = 0; i < 16; ++i) {
        const auto previous = f.commands.requests.size();
        f.server.Poll();
        if (f.commands.requests.size() - previous > 16) return false;
        for (int j = 0; j < ClientLimit; ++j)
            if (Fixture::Read(f.clients[j], replies[j])) return false;
    }
    // 即使所有客户端持续发送，全局上限也不能让任何连接得不到处理。
    for (const auto& reply : replies) if (reply.empty()) return false;
    return true;
}

bool HalfClose()
{
    Fixture f;
    const int client = f.Connect();
    if (client < 0 || !f.Send(client, "large\nsecond\n") || shutdown(client, SHUT_WR) != 0) return false;
    const std::string expected = std::string(4 * 1024 * 1024, 'L') + "\nsecond\n";
    std::string received;
    bool closed = false;
    for (int i = 0; i < 10000 && !closed; ++i) {
        f.server.Poll();
        closed = Fixture::Read(client, received);
    }
    return closed && received == expected && f.commands.disconnected.size() == 1;
}

bool MaximumResponseAndWriteBudget()
{
    InjectFaults guard;
    faults.smallBuffer = false;
    faults.sendEintr = faults.sendAgain = faults.readEintr = faults.acceptEintr = 0;
    Fixture f;
    const int client = f.Connect();
    if (client < 0 || !f.Send(client, "maximum\n") || faults.written > 64 * 1024) return false;
    std::string received;
    const std::string expected = std::string(ResponseLimit, 'M') + '\n';
    for (int i = 0; i < 10000 && received.size() < expected.size(); ++i) {
        const auto previous = faults.written;
        f.server.Poll();
        if (faults.written - previous > 64 * 1024 || Fixture::Read(client, received)) return false;
    }
    return received == expected;
}

bool InterruptedAttemptBudgets()
{
    InjectFaults guard;
    Fixture f;
    const int client = f.Connect();
    if (client < 0) return false;
    faults.acceptEintr = 1000;
    f.server.Poll();
    if (faults.acceptEintr != 996) return false;
    faults.acceptEintr = faults.readEintr = faults.sendAgain = 0;
    faults.sendEintr = 1000;
    if (!f.Send(client, "hello\n") || faults.sendEintr != 984) return false;
    faults.sendEintr = 0;
    std::string reply;
    for (int i = 0; i < 16 && reply.empty(); ++i) {
        f.server.Poll();
        if (Fixture::Read(client, reply)) return false;
    }
    return reply == "hello\n";
}

bool PendingReplyDisconnect()
{
    Fixture f;
    const int client = f.Connect();
    if (client < 0 || !f.Send(client, "large\nsecond\n")) return false;
    close(client);
    f.clients.clear();
    for (int i = 0; i < 16 && f.commands.disconnected.empty(); ++i) f.server.Poll();
    f.server.Release();
    return f.commands.disconnected.size() == 1 && f.commands.requests.size() == 1;
}

bool DeferredReplies()
{
    Fixture f;
    int wakeups = 0;
    f.server.SetReadyCallback([&] { ++wakeups; });
    const int slow = f.Connect(), fast = f.Connect();
    if (slow < 0 || fast < 0 || !f.Send(slow, "delayed\nsecond\n")) return false;
    for (int i = 0; i < 16; ++i) f.server.Poll();
    if (f.commands.pending.size() != 1 || f.commands.requests.size() != 1 || f.server.HasBufferedWork()) return false;
    if (!f.Send(fast, "fast\n")) return false;
    std::string reply;
    for (int i = 0; i < 16 && reply.empty(); ++i) { f.server.Poll(); Fixture::Read(fast, reply); }
    if (reply != "fast\n" || shutdown(slow, SHUT_WR) != 0) return false;
    const auto callback = f.commands.pending.front();
    const int before = wakeups;
    callback("done"); callback("duplicate"); // 重复完成不能覆盖结果或再次唤醒。
    if (wakeups != before + 1 || !f.server.HasBufferedWork()) return false;
    reply.clear(); bool closed = false;
    for (int i = 0; i < 32 && !closed; ++i) { f.server.Poll(); closed = Fixture::Read(slow, reply); }
    if (!closed || reply != "done\nsecond\n") return false;
    if (!f.Send(fast, "delayed\n") || f.commands.pending.size() != 2) return false;
    f.server.Release();
    const int releasedWakeups = wakeups;
    f.commands.pending.back()("late completion after transport teardown");
    return wakeups == releasedWakeups;
}
}

int main()
{
    bool ok = true;
    const auto check = [&](bool result, const char* name) {
        if (!result) { std::fprintf(stderr, "FAILED: %s\n", name); ok = false; }
    };
    check(SlowReader(false), "slow reader receives complete ordered replies while another client progresses");
    check(DeferredReplies(), "deferred replies preserve ordering, half-close and late completion safety");
    check(SlowReader(true), "small send buffer, short writes, EAGAIN and EINTR preserve replies");
    check(RequestBoundaries(), "fragmented request exactly at the line limit");
    check(RejectLongRequest(false), "unterminated request over the limit is disconnected");
    check(RejectLongRequest(true), "terminated request over the limit is not dispatched");
    check(RejectLargeResponse(), "oversized reply is disconnected without a partial frame");
    check(ConnectionLimit(), "excess connections are rejected without harming existing clients");
    check(BoundedCommands(), "bounded dispatch drains buffered commands without new socket input");
    check(AcceptBudget(), "accept attempts including EINTR are bounded per poll");
    check(GlobalFairness(), "global command budget rotates service across all clients");
    check(HalfClose(), "half-closed request stream still receives ordered pending replies");
    check(MaximumResponseAndWriteBudget(), "maximum sized reply and bounded writes per poll");
    check(InterruptedAttemptBudgets(), "repeated EINTR cannot bypass accept or send attempt budgets");
    check(PendingReplyDisconnect(), "peer disconnect with pending data releases the connection exactly once");
    return ok ? 0 : 1;
}
