#include "LAppIPC.hpp"
#include "IpcSocketPath.hpp"
#include "ExternalControlState.hpp"
#include "RuntimeLifecycle.hpp"

#include <cstdlib>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h>
#include <vector>

struct FakeCommands : ICommandService
{
    int calls = 0;
    int releases = 0;
    std::string source;
    ExternalControlState controls;
    std::string Execute(const std::string& json, const std::string& connection) override
    {
        ++calls;
        source = connection;
        if (json == "mouth") controls.SetMouth(connection, ExternalControlState::Lifetime::Stream, 0.8f);
        if (json == "look") controls.SetLook(connection, ExternalControlState::Lifetime::Stream, 0.2f, 0.3f);
        return json == "first" ? "one" : "two";
    }
    void Disconnected(const std::string& connection) override
    {
        if (connection == source) ++releases;
        controls.Release(connection);
    }
};

int main()
{
    static_assert(!std::is_copy_constructible<LAppIPC>::value, "IPC owns file descriptors");
    static_assert(!std::is_copy_assignable<LAppIPC>::value, "IPC owns file descriptors");
    if (IpcSocketPathForUid(nullptr, 1000) == IpcSocketPathForUid(nullptr, 1001) ||
        IpcSocketPathForUid("/run/user/1000", 1000) != "/run/user/1000/deskwaifu.sock") return 1;
    char path[] = "/tmp/deskwaifu-ipc-test-XXXXXX";
    if (!mkdtemp(path)) return 1;
    const char* previous = getenv("XDG_RUNTIME_DIR");
    const std::string saved = previous ? previous : "";
    setenv("XDG_RUNTIME_DIR", path, 1);

    FakeCommands commands;
    LAppIPC first;
    first.SetCommandService(commands);
    bool ok = first.Initialize();
    LAppIPC second;
    second.SetCommandService(commands);
    if (second.Initialize()) ok = false;
    int ownershipClient = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un ownershipAddr{};
    ownershipAddr.sun_family = AF_UNIX;
    std::strncpy(ownershipAddr.sun_path, first.GetSocketPath().c_str(), sizeof(ownershipAddr.sun_path) - 1);
    if (ok) ok = connect(ownershipClient, reinterpret_cast<sockaddr*>(&ownershipAddr), sizeof(ownershipAddr)) == 0;
    close(ownershipClient);
    second.Release();
    ownershipClient = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ok) ok = connect(ownershipClient, reinterpret_cast<sockaddr*>(&ownershipAddr), sizeof(ownershipAddr)) == 0;
    close(ownershipClient);

    if (unlink(first.GetSocketPath().c_str()) != 0) ok = false;
    const int replacement = socket(AF_UNIX, SOCK_STREAM, 0);
    if (replacement < 0 || bind(replacement, reinterpret_cast<sockaddr*>(&ownershipAddr), sizeof(ownershipAddr)) != 0 ||
        listen(replacement, 1) != 0) ok = false;
    first.Release();
    if (access(ownershipAddr.sun_path, F_OK) != 0) ok = false;
    if (replacement >= 0) close(replacement);
    unlink(ownershipAddr.sun_path);

    LAppIPC execOwner;
    execOwner.SetCommandService(commands);
    if (!execOwner.Initialize()) ok = false;
    int execPipe[2]{-1, -1};
    if (pipe(execPipe) != 0 || fcntl(execPipe[1], F_SETFD, FD_CLOEXEC) != 0) ok = false;
    const pid_t child = fork();
    if (child == 0) {
        close(execPipe[0]);
        execl("/bin/sh", "sh", "-c", "sleep 0.2", static_cast<char*>(nullptr));
        _exit(127);
    }
    if (child < 0) ok = false;
    close(execPipe[1]);
    char execByte;
    if (read(execPipe[0], &execByte, 1) != 0) ok = false;
    close(execPipe[0]);
    execOwner.Release();
    LAppIPC afterExec;
    afterExec.SetCommandService(commands);
    if (!afterExec.Initialize()) ok = false;
    afterExec.Release();
    if (child > 0 && waitpid(child, nullptr, 0) != child) ok = false;

    const std::string stalePath = std::string(path) + "/deskwaifu.sock";
    int stale = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un staleAddr{};
    staleAddr.sun_family = AF_UNIX;
    std::strncpy(staleAddr.sun_path, stalePath.c_str(), sizeof(staleAddr.sun_path) - 1);

    const int ordinary = open(stalePath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (ordinary < 0 || write(ordinary, "keep", 4) != 4) ok = false;
    if (ordinary >= 0) close(ordinary);
    LAppIPC occupied;
    occupied.SetCommandService(commands);
    if (occupied.Initialize()) ok = false;
    char contents[5]{};
    const int ordinaryRead = open(stalePath.c_str(), O_RDONLY);
    if (ordinaryRead < 0 || read(ordinaryRead, contents, 4) != 4 || std::string(contents) != "keep") ok = false;
    if (ordinaryRead >= 0) close(ordinaryRead);
    occupied.Release();
    unlink(staleAddr.sun_path);

    const int preparing = socket(AF_UNIX, SOCK_STREAM, 0);
    const int preparingLock = open((stalePath + ".lock").c_str(), O_CREAT | O_RDWR, 0600);
    if (preparing < 0 || preparingLock < 0 || flock(preparingLock, LOCK_EX | LOCK_NB) != 0 ||
        bind(preparing, reinterpret_cast<sockaddr*>(&staleAddr), sizeof(staleAddr)) != 0) ok = false;
    LAppIPC racing;
    racing.SetCommandService(commands);
    if (racing.Initialize() || access(staleAddr.sun_path, F_OK) != 0) ok = false;
    racing.Release();
    if (preparingLock >= 0) close(preparingLock);
    if (preparing >= 0) close(preparing);
    unlink(staleAddr.sun_path);

    const int congested = socket(AF_UNIX, SOCK_STREAM, 0);
    if (congested < 0 || bind(congested, reinterpret_cast<sockaddr*>(&staleAddr), sizeof(staleAddr)) != 0 ||
        listen(congested, 1) != 0) ok = false;
    std::vector<int> pending;
    for (int i = 0; i < 32; ++i) {
        const int client = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (client < 0) { ok = false; break; }
        const int result = connect(client, reinterpret_cast<sockaddr*>(&staleAddr), sizeof(staleAddr));
        if (result == 0 || errno == EINPROGRESS) pending.push_back(client);
        else { close(client); break; }
    }
    LAppIPC blocked;
    blocked.SetCommandService(commands);
    const auto blockedAt = std::chrono::steady_clock::now();
    if (blocked.Initialize()) ok = false;
    const auto blockedFor = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - blockedAt).count();
    if (blockedFor > 500 || access(staleAddr.sun_path, F_OK) != 0) ok = false;
    blocked.Release();
    for (int client : pending) close(client);
    if (congested >= 0) close(congested);
    unlink(staleAddr.sun_path);

    if (stale < 0 || bind(stale, reinterpret_cast<sockaddr*>(&staleAddr), sizeof(staleAddr)) != 0) ok = false;
    if (stale >= 0) close(stale);

    LAppIPC transport;
    transport.SetCommandService(commands);
    if (ok) ok = transport.Initialize();
    int client = socket(AF_UNIX, SOCK_STREAM, 0);
    timeval limit{2, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, transport.GetSocketPath().c_str(), sizeof(addr.sun_path) - 1);
    if (ok) ok = connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    if (ok) ok = write(client, "fir", 3) == 3;
    transport.Poll();
    if (commands.calls != 0) ok = false;
    if (ok) ok = write(client, "st\nsecond\n", 10) == 10;
    transport.Poll();
    char response[64]{};
    if (ok) ok = read(client, response, sizeof(response)) == 8;
    if (std::string(response) != "one\ntwo\n" || commands.calls != 2 ||
        commands.source.find("stream:") != 0) ok = false;
    const std::string firstSource = commands.source;
    close(client);
    transport.Poll();
    if (commands.releases != 1) ok = false;
    if (commands.controls.Frame().hasMouth) ok = false;
    int streaming = socket(AF_UNIX, SOCK_STREAM, 0);
    setsockopt(streaming, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
    if (ok) ok = connect(streaming, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    if (ok) ok = write(streaming, "mouth\nlook\n", 11) == 11;
    transport.Poll();
    if (ok) ok = read(streaming, response, sizeof(response)) == 8;
    auto frame = commands.controls.Frame();
    if (!frame.hasMouth || frame.mouth != 0.8f || !frame.hasLook || frame.lookX != 0.2f ||
        commands.source.find("stream:") != 0 || commands.source == firstSource) ok = false;
    close(streaming);
    transport.Poll();
    if (commands.releases != 2 || commands.controls.Frame().hasMouth ||
        commands.controls.Frame().hasLook) ok = false;

    int retained = socket(AF_UNIX, SOCK_STREAM, 0);
    setsockopt(retained, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
    if (ok) ok = connect(retained, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    if (ok) ok = write(retained, "mouth\n", 6) == 6;
    transport.Poll();
    if (ok) ok = read(retained, response, sizeof(response)) == 4;
    if (!commands.controls.Frame().hasMouth) ok = false;
    transport.Release();
    if (commands.releases != 3 || commands.controls.Frame().hasMouth ||
        commands.controls.Frame().hasLook) ok = false;
    close(retained);
    if (access(addr.sun_path, F_OK) == 0) ok = false;
    bool serviceAlive = true, disconnected = false;
    ReleaseCommandTransport(
        [&] { if (!serviceAlive) ok = false; disconnected = true; },
        [&] { if (!disconnected) ok = false; serviceAlive = false; });

    // 在流连接仍存在时测试真实传输层析构。传输层销毁后，清理和服务析构
    // 仍正常工作，两者都不需要解引用反向传输接口依赖。
    struct LifetimeCommands : FakeCommands
    {
        bool& ok;
        bool& transportGone;
        LifetimeCommands(bool& result, bool& gone) : ok(result), transportGone(gone) {}
        ~LifetimeCommands() override
        {
            if (!transportGone || releases != 1 || controls.Frame().hasMouth) ok = false;
            Disconnected(source);
        }
    };
    bool transportGone = false;
    auto* lifetime = new LifetimeCommands(ok, transportGone);
    auto* owned = new LAppIPC;
    owned->SetCommandService(*lifetime);
    if (!owned->Initialize()) ok = false;
    int live = socket(AF_UNIX, SOCK_STREAM, 0);
    setsockopt(live, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
    if (connect(live, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) ok = false;
    if (write(live, "mouth\n", 6) != 6) ok = false;
    owned->Poll();
    if (read(live, response, sizeof(response)) != 4 || !lifetime->controls.Frame().hasMouth) ok = false;
    ReleaseCommandTransport(
        [&] { delete owned; owned = nullptr; transportGone = true; },
        [&] { delete lifetime; lifetime = nullptr; });
    close(live);
    if (access(addr.sun_path, F_OK) == 0) ok = false;
    if (previous) setenv("XDG_RUNTIME_DIR", saved.c_str(), 1);
    else unsetenv("XDG_RUNTIME_DIR");
    unlink((stalePath + ".lock").c_str());
    rmdir(path);
    return ok ? 0 : 1;
}
