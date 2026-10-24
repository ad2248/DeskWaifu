#pragma once

#include <memory>
#include <string>
#include <vector>

struct RenderDeviceInfo
{
    std::string pciAddress;
    std::string name;
};

// Enumerates EGL hardware devices without creating or switching any context.
std::vector<RenderDeviceInfo> ListRenderDevices();

// Owns a device-platform EGL display (surfaceless for auto), separate from Qt's Wayland display.
// No GL object names cross this boundary: only bottom-up RGBA8 pixels do.
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
