#pragma once

#include <memory>
#include <string>
#include <vector>

struct RenderDeviceInfo
{
    std::string pciAddress;
    std::string name;
};

// 枚举 EGL 硬件设备，不创建或切换上下文。
std::vector<RenderDeviceInfo> ListRenderDevices();

// 持有独立于 Qt Wayland 显示连接的设备平台 EGL 显示连接；auto 使用无表面模式。
// 此边界只传递自下而上的 RGBA8 像素，不传递 GL 对象标识。
class OffscreenRenderer
{
public:
    OffscreenRenderer();
    ~OffscreenRenderer();
    bool Initialize(const std::string& pciDevice);
    bool MakeCurrent();
    void DoneCurrent();
    bool BindFrame(int width, int height);
    bool ReadFrame(std::vector<unsigned char>& pixels);
    unsigned int Framebuffer() const;
    int Width() const;
    int Height() const;
    bool Shutdown();
private:
    struct State;
    std::unique_ptr<State> _state;
};
