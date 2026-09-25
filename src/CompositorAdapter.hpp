#pragma once

#include "OutputController.hpp"
#include <string>
#include <vector>

enum class CompositorType { Hyprland, Sway, Generic };
enum class CapabilityStatus { Available, Unsupported, Unavailable, Failed };

template<typename T>
struct Capability
{
    CapabilityStatus status = CapabilityStatus::Unsupported;
    T value{};
    explicit operator bool() const { return status == CapabilityStatus::Available; }
};

class ICompositorTransport
{
public:
    virtual ~ICompositorTransport() = default;
    virtual Capability<std::string> Command(const std::vector<std::string>& args) = 0;
    virtual Capability<std::string> HyprlandSocket(const std::string& request) = 0;
};

class BoundedCompositorTransport : public ICompositorTransport
{
public:
    Capability<std::string> Command(const std::vector<std::string>& args) override;
    Capability<std::string> HyprlandSocket(const std::string& request) override;
};

class CompositorAdapter
{
public:
    static CompositorType Detect();
    CompositorAdapter(CompositorType kind, ICompositorTransport& transport)
        : _kind(kind), _transport(transport) {}
    Capability<std::pair<int, int>> GlobalCursor();
    Capability<std::string> FocusedOutput();
    Capability<std::vector<OutputSnapshot>> MonitorGeometry();
private:
    CompositorType _kind;
    ICompositorTransport& _transport;
};
