#include "CompositorAdapter.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/close_range.h>
#include <mutex>
#include <poll.h>
#include <regex>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {
constexpr int TimeoutMs = 250;
constexpr int ExitPollIntervalMs = 5;

// 仅接管已超时且尚未回收的子进程；工作线程持有其 PID，直到 waitpid 完成回收。
// 不等待无关子进程。
class DeferredChildReaper
{
public:
    DeferredChildReaper() : _worker([this] { Run(); }) {}
    ~DeferredChildReaper()
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _stopping = true;
        }
        _wake.notify_one();
        _worker.join();
    }

    void Add(pid_t pid)
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _children.push_back(pid);
        }
        _wake.notify_one();
    }

private:
    void Run()
    {
        std::unique_lock<std::mutex> lock(_mutex);
        while (true) {
            for (auto child = _children.begin(); child != _children.end();) {
                const pid_t result = waitpid(*child, nullptr, WNOHANG);
                if (result == *child || (result < 0 && errno == ECHILD))
                    child = _children.erase(child);
                else ++child;
            }
            // 进程退出时，内核会重新托管仍存活的子进程。
            // 不因不可中断的子进程而阻塞退出。
            if (_stopping) return;
            if (_children.empty())
                _wake.wait(lock, [this] { return _stopping || !_children.empty(); });
            else _wake.wait_for(lock, std::chrono::milliseconds(ExitPollIntervalMs));
        }
    }

    std::mutex _mutex;
    std::condition_variable _wake;
    std::vector<pid_t> _children;
    bool _stopping = false;
    std::thread _worker;
};

DeferredChildReaper& ChildReaper()
{
    static DeferredChildReaper reaper;
    return reaper;
}

bool WaitForExitBefore(pid_t pid, std::chrono::steady_clock::time_point deadline, int& status)
{
    while (true) {
        const pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) return true;
        if (result < 0 && errno != EINTR) return false;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) return false;
        poll(nullptr, 0, std::min(static_cast<int>(remaining), ExitPollIntervalMs));
    }
}

// 在父进程中枚举：多线程进程 fork 后，readdir 和 sysconf 不具备异步信号安全性。
// 此时命令管道已创建。
bool OpenDescriptors(std::vector<int>& descriptors)
{
    DIR* directory = opendir("/proc/self/fd");
    if (!directory) return false;
    const int directoryFd = dirfd(directory);
    errno = 0;
    while (const auto* entry = readdir(directory)) {
        char* end = nullptr;
        const long fd = std::strtol(entry->d_name, &end, 10);
        if (*end == '\0' && fd > STDERR_FILENO && fd != directoryFd &&
            fd <= INT_MAX) descriptors.push_back(static_cast<int>(fd));
        errno = 0;
    }
    const bool ok = errno == 0;
    closedir(directory);
    return ok;
}

bool CloseInheritedDescriptors(const std::vector<int>& descriptors, bool available,
    bool forceFallback)
{
    if (!forceFallback && syscall(SYS_close_range, STDERR_FILENO + 1, ~0U,
        CLOSE_RANGE_UNSHARE) == 0) return true;
    if (!forceFallback && errno != ENOSYS && errno != EINVAL) return false;
    if (!available) return false;
    for (int fd : descriptors) close(fd);
    return true;
}

}

Capability<std::string> ReadBoundedCompositorResponse(int fd,
    ssize_t (*reader)(int, void*, std::size_t))
{
    std::string output;
    char buffer[1024];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMs);
    while (output.size() < 16384)
    {
        pollfd pending{fd, POLLIN, 0};
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) return {CapabilityStatus::Unavailable, {}};
        const int ready = poll(&pending, 1, static_cast<int>(remaining));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0)
            return {CapabilityStatus::Unavailable, {}};
        const ssize_t size = reader(fd, buffer, std::min(sizeof(buffer), 16384 - output.size()));
        if (size == 0) return {CapabilityStatus::Available, output};
        if (size < 0) {
            if (errno == EINTR) continue;
            return {CapabilityStatus::Failed, {}};
        }
        output.append(buffer, size);
    }
    return {CapabilityStatus::Failed, {}};
}

namespace {
Capability<std::string> ReadBounded(int fd)
{
    return ReadBoundedCompositorResponse(fd, read);
}

bool Number(const std::string& json, const std::string& key, int& value)
{
    const std::regex pattern("\\\"" + key + "\\\"\\s*:\\s*(-?[0-9]+)");
    std::smatch match;
    if (!std::regex_search(json, match, pattern)) return false;
    try { value = std::stoi(match[1].str()); return true; }
    catch (...) { return false; }
}

bool String(const std::string& json, const std::string& key, std::string& value)
{
    const std::regex pattern("\\\"" + key + "\\\"\\s*:\\s*\\\"([^\\\"]+)\\\"");
    std::smatch match;
    if (!std::regex_search(json, match, pattern)) return false;
    value = match[1].str();
    return true;
}

// 只提取顶层 JSON 对象，避免其他输出设备中嵌套的 rect/name 干扰焦点结果。
std::vector<std::string> Objects(const std::string& json)
{
    std::vector<std::string> objects;
    int depth = 0;
    std::size_t start = 0;
    bool quoted = false, escaped = false;
    for (std::size_t i = 0; i < json.size(); ++i) {
        const char c = json[i];
        if (escaped) { escaped = false; continue; }
        if (c == '\\' && quoted) { escaped = true; continue; }
        if (c == '"') { quoted = !quoted; continue; }
        if (quoted) continue;
        if (c == '{') { if (depth++ == 0) start = i; }
        else if (c == '}' && depth > 0 && --depth == 0) objects.push_back(json.substr(start, i - start + 1));
    }
    return objects;
}
}

Capability<std::string> BoundedCompositorTransport::Command(const std::vector<std::string>& args)
{
    if (args.empty()) return {CapabilityStatus::Failed, {}};
    int pipes[2];
    if (pipe(pipes) != 0) return {CapabilityStatus::Failed, {}};
    std::vector<int> descriptors;
    const bool available = OpenDescriptors(descriptors);
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    const pid_t pid = fork();
    if (pid < 0) { close(pipes[0]); close(pipes[1]); return {CapabilityStatus::Failed, {}}; }
    if (!pid) {
        setpgid(0, 0);
        close(pipes[0]);
        dup2(pipes[1], STDOUT_FILENO);
        close(pipes[1]);
        if (!CloseInheritedDescriptors(descriptors, available, _forceDescriptorFallback)) _exit(127);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    setpgid(pid, pid);
    close(pipes[1]);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMs);
    auto output = ReadBounded(pipes[0]);
    close(pipes[0]);
    int status = 0;
    if (!output || !WaitForExitBefore(pid, deadline, status)) {
        output.status = CapabilityStatus::Unavailable;
        kill(-pid, SIGKILL);
        // 直接子进程可能脱离原进程组，因此也要向其 PID 发送信号。
        // 信号发送失败后，不能无限等待。
        int killed;
        do { killed = kill(pid, SIGKILL); } while (killed < 0 && errno == EINTR);
        if (killed < 0 && errno != ESRCH)
            std::fprintf(stderr, "[Compositor] Failed to terminate child: %s\n", std::strerror(errno));
        const auto cleanupDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMs);
        if (!WaitForExitBefore(pid, cleanupDeadline, status)) {
            std::fprintf(stderr, "[Compositor] Deferring child reaping after cleanup deadline\n");
            ChildReaper().Add(pid);
        }
    }
    if (output && (!WIFEXITED(status) || WEXITSTATUS(status))) output.status = CapabilityStatus::Failed;
    return output;
}

Capability<std::string> BoundedCompositorTransport::HyprlandSocket(const std::string& request)
{
    const char* sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char* runtime = getenv("XDG_RUNTIME_DIR");
    if (!sig || !runtime) return {CapabilityStatus::Unavailable, {}};
    const std::string path = std::string(runtime) + "/hypr/" + sig + "/.socket.sock";
    if (path.size() >= sizeof(sockaddr_un::sun_path)) return {CapabilityStatus::Failed, {}};
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return {CapabilityStatus::Failed, {}};
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, path.c_str());
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 && errno != EINPROGRESS) {
        close(fd); return {CapabilityStatus::Unavailable, {}};
    }
    pollfd ready{fd, POLLOUT, 0};
    if (poll(&ready, 1, TimeoutMs) <= 0) { close(fd); return {CapabilityStatus::Unavailable, {}}; }
    int error = 0;
    socklen_t length = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error) {
        close(fd); return {CapabilityStatus::Unavailable, {}};
    }
    if (send(fd, request.c_str(), request.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(request.size())) {
        close(fd); return {CapabilityStatus::Failed, {}};
    }
    auto output = ReadBounded(fd);
    close(fd);
    return output;
}

CompositorType CompositorAdapter::Detect()
{
    if (getenv("HYPRLAND_INSTANCE_SIGNATURE")) return CompositorType::Hyprland;
    if (getenv("SWAYSOCK") || getenv("I3SOCK")) return CompositorType::Sway;
    return CompositorType::Generic;
}

Capability<std::pair<int, int>> CompositorAdapter::GlobalCursor()
{
    if (_kind != CompositorType::Hyprland) return {CapabilityStatus::Unsupported, {}};
    const auto output = _transport.HyprlandSocket("-j/cursorpos");
    if (!output) return {output.status, {}};
    int x = 0, y = 0;
    if (!Number(output.value, "x", x) || !Number(output.value, "y", y))
        return {CapabilityStatus::Failed, {}};
    return {CapabilityStatus::Available, {x, y}};
}

Capability<std::pair<int, int>> CompositorAdapter::TrackedCursor(std::chrono::steady_clock::time_point now)
{
    if (!_cursorThrottle.Ready(now)) return {CapabilityStatus::Unavailable, {}};
    return GlobalCursor();
}

Capability<std::string> CompositorAdapter::FocusedOutput()
{
    if (_kind == CompositorType::Generic) return {CapabilityStatus::Unsupported, {}};
    auto output = _transport.Command(_kind == CompositorType::Hyprland
        ? std::vector<std::string>{"hyprctl", "monitors", "-j"}
        : std::vector<std::string>{"swaymsg", "-t", "get_outputs"});
    if (!output) return output;
    for (const auto& object : Objects(output.value)) {
        if (!std::regex_search(object, std::regex("\\\"focused\\\"\\s*:\\s*true"))) continue;
        std::string name;
        if (String(object, "name", name)) return {CapabilityStatus::Available, name};
    }
    return {CapabilityStatus::Failed, {}};
}

Capability<std::vector<OutputSnapshot>> CompositorAdapter::MonitorGeometry()
{
    if (_kind != CompositorType::Hyprland) return {CapabilityStatus::Unsupported, {}};
    auto result = _transport.Command({"hyprctl", "monitors", "-j"});
    if (!result) return {result.status, {}};
    std::vector<OutputSnapshot> outputs;
    for (const auto& object : Objects(result.value)) {
        OutputSnapshot output{};
        if (String(object, "name", output.name) && Number(object, "x", output.x) &&
            Number(object, "y", output.y)) outputs.push_back(output);
    }
    return outputs.empty() ? Capability<std::vector<OutputSnapshot>>{CapabilityStatus::Failed, {}}
        : Capability<std::vector<OutputSnapshot>>{CapabilityStatus::Available, outputs};
}
