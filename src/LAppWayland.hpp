#include <vector>
#pragma once

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "ModelKeyboardGrab.hpp"
#include "WaylandEvents.hpp"
#include "OutputController.hpp"

#define namespace wayland_namespace
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#undef namespace
#include "xdg-shell-client-protocol.h"

struct WaylandContext {
    IWaylandEvents* events = nullptr; // borrowed, owned by application
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    wl_seat* seat = nullptr;
    wl_pointer* pointer = nullptr;
    wl_keyboard* keyboard = nullptr;
    KeyboardSurfaceLifecycle keyboardSurface;
    zwlr_layer_shell_v1* layer_shell = nullptr;

    wl_surface* surface = nullptr;
    zwlr_layer_surface_v1* layer_surface = nullptr;
    wl_egl_window* egl_window = nullptr;

    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLContext egl_context = EGL_NO_CONTEXT;
    EGLSurface egl_surface = EGL_NO_SURFACE;
    EGLConfig egl_config = nullptr;

    struct OutputInfo {
    WaylandContext* owner = nullptr;
    uint32_t id = 0; // wl_registry global name, stable until global_remove
    wl_output* output;
    int x;
    int y;
    int width;
    int height;
    char name[32];
};
    std::vector<OutputInfo*> outputs;
    OutputId current_output_id = 0;
    bool outputNeedsRebind = false;
    bool protocolFailed = false;
    int width = 0;
    int height = 0;
    int margin_left = 0;
    int margin_top = 0;
};

bool SetupWaylandContext(WaylandContext* wl, int width, int height);
void CleanWaylandContext(WaylandContext* wl);
bool MakeWaylandGpuContextCurrent(WaylandContext* wl);
class WaylandBackend : public IOutputBackend
{
public:
    explicit WaylandBackend(WaylandContext& context) : _wl(context) {}
    std::vector<OutputSnapshot> Outputs() const override;
    OutputId CurrentOutput() const override { return _wl.current_output_id; }
    bool SwitchOutput(OutputId id) override;
    void ReconcileOutputs();
    void ApplyMonitorGeometry(const std::vector<OutputSnapshot>& monitors);
private:
    WaylandContext& _wl;
};
KeyboardSurfaceLifecycle::GrabResult SetModelKeyboardGrab(WaylandContext* wl, bool grabbed);
