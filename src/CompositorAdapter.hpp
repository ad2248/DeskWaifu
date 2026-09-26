#pragma once

#include "OutputController.hpp"
#include <chrono>
#include <string>
#include <vector>
#include <sys/types.h>

enum class CompositorType { Hyprland, Sway, Generic };
enum class CapabilityStatus { Available, Unsupported, Unavailable, Failed };

template<typename T>
struct Capability
{
    CapabilityStatus status = CapabilityStatus::Unsupported;
    T value{};
    explicit operator bool() const { return status == CapabilityStatus::Available; }
};

class CursorQueryThrottle
{
public:
    explicit CursorQueryThrottle(std::chrono::milliseconds interval = std::chrono::milliseconds(30))
        : _interval(interval) {}

    bool Ready(std::chrono::steady_clock::time_point now)
    {
        if (!_queried || now - _lastQuery >= _interval) {
            _lastQuery = now;
            _queried = true;
            return true;
        }
        return false;
    }

private:
    std::chrono::milliseconds _interval;
    std::chrono::steady_clock::time_point _lastQuery{};
    bool _queried = false;
};

class ICompositorTransport
{
public:
    virtual ~ICompositorTransport() = default;
    virtual Capability<std::string> Command(const std::vector<std::string>& args) = 0;
    virtual Capability<std::string> HyprlandSocket(const std::string& request) = 0;
};

// Reader injection makes the poll-ready/read-interrupted transition testable.
Capability<std::string> ReadBoundedCompositorResponse(int fd,
    ssize_t (*reader)(int, void*, std::size_t));

class BoundedCompositorTransport : public ICompositorTransport
{
public:
    explicit BoundedCompositorTransport(bool forceDescriptorFallback = false)
        : _forceDescriptorFallback(forceDescriptorFallback) {}
    Capability<std::string> Command(const std::vector<std::string>& args) override;
    Capability<std::string> HyprlandSocket(const std::string& request) override;
private:
    bool _forceDescriptorFallback;
};

class CompositorAdapter
{
public:
    static CompositorType Detect();
    CompositorAdapter(CompositorType kind, ICompositorTransport& transport,
        std::chrono::milliseconds interval = std::chrono::milliseconds(30))
        : _kind(kind), _transport(transport), _cursorThrottle(interval) {}
    Capability<std::pair<int, int>> GlobalCursor();
    Capability<std::pair<int, int>> TrackedCursor(std::chrono::steady_clock::time_point now);
    Capability<std::string> FocusedOutput();
    Capability<std::vector<OutputSnapshot>> MonitorGeometry();
private:
    CompositorType _kind;
    ICompositorTransport& _transport;
    CursorQueryThrottle _cursorThrottle;
};
