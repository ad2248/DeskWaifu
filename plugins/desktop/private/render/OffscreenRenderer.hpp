#pragma once

#include <memory>
#include <string>
#include <vector>

// EGL 硬件设备的稳定 PCI 地址及供用户显示的名称。
struct RenderDeviceInfo {
    std::string pciAddress;
    std::string name;
};

// 枚举 EGL 硬件设备，不创建或切换上下文。
std::vector<RenderDeviceInfo> ListRenderDevices();

// 持有独立于 Qt Wayland 显示连接的设备平台 EGL 显示连接；auto 使用无表面模式。
// 此边界只传递自下而上的 RGBA8 像素，不传递 GL 对象标识。
class OffscreenRenderer {
public:
    OffscreenRenderer();
    ~OffscreenRenderer();
    // 选择 PCI 设备并创建当前上下文；空字符串或 auto 使用自动选择，失败返回 false。
    bool Initialize(const std::string& pciDevice);
    // 将自有上下文绑定到调用线程，失败返回 false；不得同时在多个线程使用。
    bool MakeCurrent();
    // 仅当本对象的上下文为当前上下文时解除绑定。
    void DoneCurrent();
    // 在当前上下文创建或调整帧缓冲并绑定；尺寸无效或 GL 失败返回 false。
    bool BindFrame(int width, int height);
    // 读回当前帧的 RGBA8 数据，失败返回 false；调用方拥有输出缓冲。
    bool ReadFrame(std::vector<unsigned char>& pixels);
    unsigned int Framebuffer() const;
    int Width() const;
    int Height() const;
    // 重新绑定自有上下文并释放全部 EGL/GL 资源；无法绑定以销毁 GL 资源时返回 false。
    // 显式关闭及析构应在渲染线程完成，且此前不得终止依赖的 EGL 连接。
    bool Shutdown();

private:
    struct State;
    std::unique_ptr<State> _state;
};
