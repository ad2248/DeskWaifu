#include "CompositorAdapter.hpp"
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>

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

int main()
{
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
    if (actual.Command({"/definitely/missing/compositor"}).status != CapabilityStatus::Failed) return 1;
    const int inherited = fcntl(STDOUT_FILENO, F_DUPFD, 100);
    if (inherited < 0) return 1;
    const auto descriptorCheck = actual.Command({"sh", "-c",
        "test ! -e /proc/self/fd/" + std::to_string(inherited)});
    close(inherited);
    if (!descriptorCheck) return 1;

    struct sigaction interrupt{}, previous{};
    interrupt.sa_handler = InterruptPoll;
    sigemptyset(&interrupt.sa_mask);
    if (sigaction(SIGALRM, &interrupt, &previous) != 0) return 1;
    ualarm(50000, 0);
    const auto interrupted = actual.Command({"sh", "-c", "sleep 0.15; printf recovered"});
    ualarm(0, 0);
    if (sigaction(SIGALRM, &previous, nullptr) != 0) return 1;
    if (!interrupted || interrupted.value != "recovered") return 1;
    return 0;
}
