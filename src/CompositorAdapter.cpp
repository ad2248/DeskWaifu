#include "CompositorAdapter.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <linux/close_range.h>
#include <poll.h>
#include <regex>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
constexpr int TimeoutMs = 250;

void CloseInheritedDescriptors()
{
    if (syscall(SYS_close_range, STDERR_FILENO + 1, ~0U, CLOSE_RANGE_UNSHARE) == 0) return;
    const long limit = sysconf(_SC_OPEN_MAX);
    const int maximum = limit > STDERR_FILENO
        ? static_cast<int>(std::min(limit, static_cast<long>(std::numeric_limits<int>::max())))
        : 1024;
    for (int fd = STDERR_FILENO + 1; fd < maximum; ++fd) close(fd);
}

Capability<std::string> ReadBounded(int fd)
{
    std::string output;
    char buffer[1024];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMs);
    while (output.size() < 16384)
    {
        pollfd pending{fd, POLLIN, 0};
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0 || poll(&pending, 1, static_cast<int>(remaining)) <= 0)
            return {CapabilityStatus::Unavailable, {}};
        const ssize_t size = read(fd, buffer, std::min(sizeof(buffer), 16384 - output.size()));
        if (size == 0) return {CapabilityStatus::Available, output};
        if (size < 0) {
            if (errno == EINTR) continue;
            return {CapabilityStatus::Failed, {}};
        }
        output.append(buffer, size);
    }
    return {CapabilityStatus::Failed, {}};
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

// Extract top-level JSON objects without letting a nested rect/name from a
// different output affect a focused result.
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
    const pid_t pid = fork();
    if (pid < 0) { close(pipes[0]); close(pipes[1]); return {CapabilityStatus::Failed, {}}; }
    if (!pid) {
        setpgid(0, 0);
        close(pipes[0]);
        dup2(pipes[1], STDOUT_FILENO);
        close(pipes[1]);
        CloseInheritedDescriptors();
        std::vector<char*> argv;
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    setpgid(pid, pid);
    close(pipes[1]);
    auto output = ReadBounded(pipes[0]);
    close(pipes[0]);
    if (!output) kill(-pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
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
