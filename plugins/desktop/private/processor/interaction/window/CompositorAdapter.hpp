#pragma once

#include "OutputController.hpp"
#include <chrono>
#include <string>
#include <sys/types.h>
#include <vector>

// 按环境探测的合成器类别；Generic 只保留通用窗口能力。
enum class CompositorType { Hyprland, Sway, Generic };
// 分别表示可用、不支持、暂不可用及请求或解析失败。
enum class CapabilityStatus { Available, Unsupported, Unavailable, Failed };

// 带能力状态的结果；仅在转换为 bool 为 true 时读取 value。
template <typename T> struct Capability {
    CapabilityStatus status = CapabilityStatus::Unsupported;
    T value { };
    explicit operator bool() const { return status == CapabilityStatus::Available; }
};

// 使用单调时钟限制查询频率；Ready() 返回 true 时同时记录本次查询。
class CursorQueryThrottle {
public:
    explicit CursorQueryThrottle(std::chrono::milliseconds interval = std::chrono::milliseconds(30))
        : _interval(interval)
    {
    }

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
    std::chrono::steady_clock::time_point _lastQuery { };
    bool _queried = false;
};

// 将合成器查询与进程、套接字传输解耦，便于注入测试实现。
class ICompositorTransport {
public:
    virtual ~ICompositorTransport() = default;
    // 按参数数组执行命令，返回标准输出或能力错误，不经过 shell 展开。
    virtual Capability<std::string> Command(const std::vector<std::string>& args) = 0;
    // 通过 Hyprland 控制套接字发送请求并读取响应。
    virtual Capability<std::string> HyprlandSocket(const std::string& request) = 0;
};

// 注入读取函数，以便测试 poll 就绪后读取被中断的情况。
Capability<std::string> ReadBoundedCompositorResponse(int fd, ssize_t (*reader)(int, void*, std::size_t));

// 有界等待及有界响应的同步传输；命令超时后终止并回收子进程。
// forceDescriptorFallback 用于验证不依赖 close_range 的描述符清理路径。
class BoundedCompositorTransport : public ICompositorTransport {
public:
    explicit BoundedCompositorTransport(bool forceDescriptorFallback = false)
        : _forceDescriptorFallback(forceDescriptorFallback)
    {
    }
    Capability<std::string> Command(const std::vector<std::string>& args) override;
    Capability<std::string> HyprlandSocket(const std::string& request) override;

private:
    bool _forceDescriptorFallback;
};

// 统一查询合成器扩展能力；借用 transport，在 GUI 交互域串行调用。
// 不支持的合成器通过状态报告缺失能力，不伪造空的成功结果。
class CompositorAdapter {
public:
    // 从环境变量探测类别，Hyprland 优先于 Sway，否则为 Generic。
    static CompositorType Detect();
    CompositorAdapter(CompositorType kind, ICompositorTransport& transport, std::chrono::milliseconds interval = std::chrono::milliseconds(30))
        : _kind(kind)
        , _transport(transport)
        , _cursorThrottle(interval)
    {
    }
    // 查询全局逻辑像素光标位置；目前仅 Hyprland 支持。
    Capability<std::pair<int, int>> GlobalCursor();
    // 节流后的全局光标查询；未到查询间隔时返回 Unavailable 而非缓存位置。
    Capability<std::pair<int, int>> TrackedCursor(std::chrono::steady_clock::time_point now);
    // 查询 Hyprland 或 Sway 的焦点输出名称，失败状态保留在返回值中。
    Capability<std::string> FocusedOutput();
    // 返回 Hyprland 输出的名称及全局原点；ID 和尺寸未填充，不能替代后端枚举。
    Capability<std::vector<OutputSnapshot>> MonitorGeometry();

private:
    CompositorType _kind;
    ICompositorTransport& _transport;
    CursorQueryThrottle _cursorThrottle;
};
