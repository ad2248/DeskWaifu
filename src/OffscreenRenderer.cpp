#include "OffscreenRenderer.hpp"
#include "RenderDevice.hpp"
#include "ReadRgbaFrame.hpp"
#include <GL/glew.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits.h>
#include <algorithm>
#include <fstream>

struct OffscreenRenderer::State
{
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;
    GLuint framebuffer = 0, texture = 0, depth = 0;
    int width = 0, height = 0;
};

namespace {
struct Device
{
    EGLDeviceEXT handle;
    RenderDeviceInfo info;
};

std::vector<Device> EnumerateDevices()
{
    std::vector<Device> result;
    const auto queryDevices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
    const auto queryString = reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(eglGetProcAddress("eglQueryDeviceStringEXT"));
    EGLint count = 0;
    if (!queryDevices || !queryString || !queryDevices(0, nullptr, &count)) {
        std::cerr << "[GPU] EGL device enumeration is unavailable\n";
        return result;
    }
    std::vector<EGLDeviceEXT> devices(count);
    if (!queryDevices(count, devices.data(), &count)) return result;
    for (int i = 0; i < count; ++i) {
        const char* extensions = queryString(devices[i], EGL_EXTENSIONS);
        if (!extensions || !std::strstr(extensions, "EGL_EXT_device_drm_render_node")) continue;
        const char* node = queryString(devices[i], EGL_DRM_RENDER_NODE_FILE_EXT);
        if (!node) continue;
        const std::string path(node);
        char resolved[PATH_MAX];
        const std::string sysfs = "/sys/class/drm/" + path.substr(path.find_last_of('/') + 1) + "/device";
        if (!realpath(sysfs.c_str(), resolved)) continue;
        const std::string devicePath(resolved);
        const auto pciAddress = devicePath.substr(devicePath.find_last_of('/') + 1);
        if (!ValidRenderDevice(pciAddress)) continue;
        std::string name;
        if (std::strstr(extensions, "EGL_EXT_device_query_name")) {
            const char* renderer = queryString(devices[i], EGL_RENDERER_EXT);
            if (renderer) name = renderer;
        }
        if (name.empty()) {
            std::ifstream vendorFile(devicePath + "/vendor");
            std::string vendor; vendorFile >> vendor;
            name = vendor == "0x8086" ? "Intel" : vendor == "0x10de" ? "NVIDIA" : vendor == "0x1002" ? "AMD" : "GPU";
        }
        result.push_back({devices[i], {pciAddress, name}});
    }
    std::sort(result.begin(), result.end(), [](const Device& a, const Device& b) { return a.info.pciAddress < b.info.pciAddress; });
    result.erase(std::unique(result.begin(), result.end(), [](const Device& a, const Device& b) {
        return a.info.pciAddress == b.info.pciAddress;
    }), result.end());
    return result;
}
}

std::vector<RenderDeviceInfo> ListRenderDevices()
{
    std::vector<RenderDeviceInfo> result;
    for (const auto& device : EnumerateDevices()) result.push_back(device.info);
    return result;
}

OffscreenRenderer::OffscreenRenderer() : _state(new State) {}
OffscreenRenderer::~OffscreenRenderer() { Shutdown(); }

bool OffscreenRenderer::Initialize(const std::string& pciDevice)
{
    if (!ValidRenderDevice(pciDevice)) return false;
    const auto getDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (!getDisplay) return false;
    const bool automatic = pciDevice.empty() || pciDevice == "auto";
    EGLDeviceEXT selected = EGL_NO_DEVICE_EXT;
    if (!automatic) for (const auto& device : EnumerateDevices()) if (device.info.pciAddress == pciDevice) selected = device.handle;
    if (!automatic && selected == EGL_NO_DEVICE_EXT) {
        std::cerr << "[GPU] Requested PCI device is unavailable: " << pciDevice << '\n';
        return false;
    }
    auto& s = *_state;
    s.display = automatic ? getDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
        : getDisplay(EGL_PLATFORM_DEVICE_EXT, selected, nullptr);
    if (s.display == EGL_NO_DISPLAY || !eglInitialize(s.display, nullptr, nullptr) || !eglBindAPI(EGL_OPENGL_API)) {
        std::cerr << "[GPU] Cannot initialize device EGL display\n";
        return false;
    }
    const EGLint attributes[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig config;
    EGLint found = 0;
    if (!eglChooseConfig(s.display, attributes, &config, 1, &found) || !found) {
        std::cerr << "[GPU] No device EGL config supports OpenGL pbuffer rendering\n";
        return false;
    }
    const EGLint contextAttributes[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT, EGL_NONE};
    s.context = eglCreateContext(s.display, config, EGL_NO_CONTEXT, contextAttributes);
    const EGLint surfaceAttributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
    s.surface = eglCreatePbufferSurface(s.display, config, surfaceAttributes);
    if (s.context == EGL_NO_CONTEXT || s.surface == EGL_NO_SURFACE || !MakeCurrent()) {
        std::cerr << "[GPU] Cannot create desktop OpenGL 3.3 compatibility offscreen context\n";
        return false;
    }
    glewExperimental = GL_TRUE;
    const auto glewError = glewInit();
    if (glewError != GLEW_OK) {
        std::cerr << "[GPU] Cannot initialize offscreen OpenGL functions: " << glewGetErrorString(glewError) << '\n';
        return false;
    }
    // GLEW 探测可能遗留错误，不能将其误判为渲染错误。
    while (glGetError() != GL_NO_ERROR) {}
    std::cout << "[GPU] Render device " << pciDevice << ": " << glGetString(GL_VENDOR)
        << " / " << glGetString(GL_RENDERER) << " / " << glGetString(GL_VERSION) << std::endl;
    return true;
}

bool OffscreenRenderer::MakeCurrent()
{
    auto& s = *_state;
    return s.display != EGL_NO_DISPLAY && s.context != EGL_NO_CONTEXT && s.surface != EGL_NO_SURFACE
        && eglMakeCurrent(s.display, s.surface, s.surface, s.context);
}

void OffscreenRenderer::DoneCurrent()
{
    if (_state->display != EGL_NO_DISPLAY && eglGetCurrentContext() == _state->context)
        eglMakeCurrent(_state->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

bool OffscreenRenderer::BindFrame(int width, int height)
{
    auto& s = *_state;
    if (width <= 0 || height <= 0) return false;
    if (!s.framebuffer) {
        glGenFramebuffers(1, &s.framebuffer);
        glGenTextures(1, &s.texture);
        glGenRenderbuffers(1, &s.depth);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s.framebuffer);
    if (s.width != width || s.height != height) {
        GLint maxSize = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxSize);
        if (width > maxSize || height > maxSize) return false;
        glBindTexture(GL_TEXTURE_2D, s.texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.texture, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, s.depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s.depth);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        s.width = width; s.height = height;
    }
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE && glGetError() == GL_NO_ERROR;
}

bool OffscreenRenderer::ReadFrame(std::vector<unsigned char>& pixels)
{
    auto& s = *_state;
    return ReadRgbaFrame(s.framebuffer, s.width, s.height, pixels);
}

unsigned int OffscreenRenderer::Framebuffer() const { return _state->framebuffer; }
int OffscreenRenderer::Width() const { return _state->width; }
int OffscreenRenderer::Height() const { return _state->height; }

bool OffscreenRenderer::Shutdown()
{
    auto& s = *_state;
    if (s.framebuffer || s.texture || s.depth) {
        if (!MakeCurrent()) return false;
        glDeleteFramebuffers(1, &s.framebuffer);
        glDeleteTextures(1, &s.texture);
        glDeleteRenderbuffers(1, &s.depth);
    }
    DoneCurrent();
    if (s.context != EGL_NO_CONTEXT) eglDestroyContext(s.display, s.context);
    if (s.surface != EGL_NO_SURFACE) eglDestroySurface(s.display, s.surface);
    if (s.display != EGL_NO_DISPLAY) eglTerminate(s.display);
    s = State{};
    return true;
}
