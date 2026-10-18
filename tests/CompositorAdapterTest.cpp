#include "CompositorAdapter.hpp"
#include <chrono>
#include <csignal>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdlib>
#include <atomic>
#include <thread>

namespace {
std::atomic<bool> delayReaping{false};
std::atomic<pid_t> delayedPid{0};
std::atomic<bool> eventuallyReaped{false};
std::chrono::steady_clock::time_point reapAfter;
}

extern "C" pid_t __real_waitpid(pid_t, int*, int);
extern "C" pid_t __wrap_waitpid(pid_t pid, int* status, int options)
{
    if (delayReaping && pid > 0) {
        pid_t unset = 0;
        delayedPid.compare_exchange_strong(unset, pid);
        if (pid == delayedPid && std::chrono::steady_clock::now() < reapAfter) return 0;
    }
    const pid_t result = __real_waitpid(pid, status, options);
    if (result > 0 && result == delayedPid) eventuallyReaped = true;
    return result;
}

namespace {
void InterruptPoll(int) {}
}

struct FakeTransport : ICompositorTransport
{
    Capability<std::string> next{CapabilityStatus::Available,
        "[{\"name\":\"A\",\"focused\":false,\"x\":0,\"y\":0},"
        "{\"name\":\"B\",\"focused\":true,\"x\":100,\"y\":200}]"};
    Capability<std::string> Command(const std::vector<std::string>&) override { return next; }
    Capability<std::string> HyprlandSocket(const std::string&) override { return next; }
};

int main(int argc, char** argv)
{
    if (argc == 2 && std::string(argv[1]) == "--escape-group") {
        if (setpgid(0, getpgid(getppid())) != 0) return 125;
        close(STDOUT_FILENO);
        sleep(1);
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--check-fd")
        return fcntl(std::atoi(argv[2]), F_GETFD) == -1 && errno == EBADF ? 0 : 1;
    const auto start = std::chrono::steady_clock::now();
    CursorQueryThrottle throttle(std::chrono::milliseconds(30));
    if (!throttle.Ready(start) || throttle.Ready(start + std::chrono::milliseconds(29)) ||
        !throttle.Ready(start + std::chrono::milliseconds(30))) return 1;

    FakeTransport transport;
    CompositorAdapter generic(CompositorType::Generic, transport);
    if (generic.GlobalCursor().status != CapabilityStatus::Unsupported ||
        generic.FocusedOutput().status != CapabilityStatus::Unsupported) return 1;
    CompositorAdapter sway(CompositorType::Sway, transport);
    if (sway.FocusedOutput().value != "B" ||
        sway.GlobalCursor().status != CapabilityStatus::Unsupported) return 1;
    CompositorAdapter hypr(CompositorType::Hyprland, transport);
    auto geometry = hypr.MonitorGeometry();
    if (!geometry || geometry.value.size() != 2 || geometry.value[1].x != 100) return 1;
    transport.next = {CapabilityStatus::Available, "{\"x\":-12,\"y\":42}"};
    if (!hypr.GlobalCursor() || hypr.GlobalCursor().value.first != -12) return 1;
    struct CountingTransport : FakeTransport {
        int queries = 0;
        Capability<std::string> HyprlandSocket(const std::string& request) override {
            ++queries;
            return FakeTransport::HyprlandSocket(request);
        }
    } counted;
    counted.next = {CapabilityStatus::Available, "{\"x\":1,\"y\":2}"};
    CompositorAdapter tracked(CompositorType::Hyprland, counted);
    if (!tracked.TrackedCursor(start) || tracked.TrackedCursor(start + std::chrono::milliseconds(29)) ||
        !tracked.TrackedCursor(start + std::chrono::milliseconds(30)) || counted.queries != 2) return 1;
    transport.next = {CapabilityStatus::Unavailable, {}};
    if (hypr.GlobalCursor().status != CapabilityStatus::Unavailable) return 1;
    transport.next = {CapabilityStatus::Available, "not json"};
    if (hypr.GlobalCursor().status != CapabilityStatus::Failed) return 1;

    BoundedCompositorTransport actual;
    const auto begin = std::chrono::steady_clock::now();
    const auto timed = actual.Command({"sh", "-c", "sleep 2"});
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin).count();
    if (timed.status != CapabilityStatus::Unavailable || elapsed > 1500) return 1;
    const auto eofBegin = std::chrono::steady_clock::now();
    const auto earlyEof = actual.Command({"sh", "-c", "exec 1>&-; sleep 1"});
    const auto eofElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - eofBegin).count();
    if (earlyEof.status != CapabilityStatus::Unavailable || eofElapsed > 750) return 1;
    const auto shortEofBegin = std::chrono::steady_clock::now();
    const auto shortEof = actual.Command({"sh", "-c", "exec 1>&-; sleep 0.01"});
    const auto shortEofElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - shortEofBegin).count();
    if (!shortEof || shortEofElapsed > 150) return 1;
    const auto escapedBegin = std::chrono::steady_clock::now();
    const auto escaped = actual.Command({argv[0], "--escape-group"});
    const auto escapedElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - escapedBegin).count();
    int childStatus = 0;
    if (escaped.status != CapabilityStatus::Unavailable || escapedElapsed > 750 ||
        waitpid(-1, &childStatus, WNOHANG) != -1 || errno != ECHILD) return 1;
    reapAfter = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
    delayReaping = true;
    const auto delayedBegin = std::chrono::steady_clock::now();
    const auto delayed = actual.Command({argv[0], "--escape-group"});
    const auto delayedElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - delayedBegin).count();
    if (delayed.status != CapabilityStatus::Unavailable || delayedElapsed > 750 || eventuallyReaped) return 1;
    const auto reapLimit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!eventuallyReaped && std::chrono::steady_clock::now() < reapLimit)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    delayReaping = false;
    if (!eventuallyReaped || waitpid(delayedPid, &childStatus, WNOHANG) != -1 || errno != ECHILD) return 1;
    if (actual.Command({"/definitely/missing/compositor"}).status != CapabilityStatus::Failed) return 1;
    struct stat procFd{};
    if (stat("/proc/self/fd/1", &procFd) != 0) return 1;
    BoundedCompositorTransport fallback(true);
    for (int minimum : {STDERR_FILENO + 1, 100}) {
        const int inherited = fcntl(STDOUT_FILENO, F_DUPFD, minimum);
        if (inherited < 0) return 1;
        const auto path = "/proc/self/fd/" + std::to_string(inherited);
        if (stat(path.c_str(), &procFd) != 0) return 1;
        const auto closed = fallback.Command({argv[0], "--check-fd", std::to_string(inherited)});
        close(inherited);
        if (!closed) return 1;
    }

    struct sigaction interrupt{}, previous{};
    interrupt.sa_handler = InterruptPoll;
    sigemptyset(&interrupt.sa_mask);
    if (sigaction(SIGALRM, &interrupt, &previous) != 0) return 1;
    ualarm(50000, 0);
    const auto interrupted = actual.Command({"sh", "-c", "sleep 0.15; printf recovered"});
    ualarm(0, 0);
    if (sigaction(SIGALRM, &previous, nullptr) != 0) return 1;
    if (!interrupted || interrupted.value != "recovered") return 1;
    int pipeFds[2];
    if (pipe(pipeFds) != 0 || write(pipeFds[1], "read-recovered", 14) != 14) return 1;
    close(pipeFds[1]);
    static bool firstRead = true;
    const auto result = ReadBoundedCompositorResponse(pipeFds[0],
        [](int fd, void* buffer, std::size_t count) -> ssize_t {
            if (firstRead) { firstRead = false; errno = EINTR; return -1; }
            return read(fd, buffer, count);
        });
    close(pipeFds[0]);
    if (!result || result.value != "read-recovered") return 1;
    return 0;
}
