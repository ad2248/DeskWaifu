#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include "LAppWayland.hpp"
#include "LAppPal.hpp"
#include "ModelKeyboardGrab.hpp"
#include <linux/input-event-codes.h>
#include <iostream>

static void ApplyLayerKeyboardInteractivity(WaylandContext* wl) {
    zwlr_layer_surface_v1_set_keyboard_interactivity(wl->layer_surface,
        wl->keyboardSurface.IsGrabbed() ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
                            : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
}

// --- Wayland Pointer Handlers ---
static void pointer_handle_enter(void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y) {
    auto* wl = static_cast<WaylandContext*>(data);
    if (wl->events) wl->events->PointerEntered(wl_fixed_to_double(surface_x), wl_fixed_to_double(surface_y));
}
static void pointer_handle_leave(void *, struct wl_pointer *, uint32_t, struct wl_surface *) {}
static void pointer_handle_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {
    double x = wl_fixed_to_double(surface_x);
    double y = wl_fixed_to_double(surface_y);
    auto* wl = static_cast<WaylandContext*>(data);
    if (wl->events) wl->events->PointerMoved(x, y);
}
static void pointer_handle_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    int glfw_action = (state == 1) ? 1 : 0; // PRESS=1, RELEASE=0 (matches GLFW definitions)
    int glfw_btn = -1;
    if (button == BTN_LEFT) glfw_btn = 0; // MOUSE_BUTTON_LEFT
    else if (button == BTN_RIGHT) glfw_btn = 1; // MOUSE_BUTTON_RIGHT
    else if (button == BTN_MIDDLE) glfw_btn = 2; // MOUSE_BUTTON_MIDDLE
    
    auto* wl = static_cast<WaylandContext*>(data);
    if (glfw_btn != -1 && wl->events) wl->events->PointerButton(glfw_btn, glfw_action == 1);
}
static void pointer_handle_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time, uint32_t axis, wl_fixed_t value) {
    double offset = wl_fixed_to_double(value);
    auto* wl = static_cast<WaylandContext*>(data);
    if (wl->events) wl->events->PointerScroll(offset > 0 ? -1 : 1);
}

static const struct wl_pointer_listener pointer_listener = {
    pointer_handle_enter,
    pointer_handle_leave,
    pointer_handle_motion,
    pointer_handle_button,
    pointer_handle_axis,
};

// Keycodes in wl_keyboard.key are evdev codes (not XKB keysyms).
static void keyboard_keymap(void*, wl_keyboard*, uint32_t, int fd, uint32_t) { close(fd); }
static void keyboard_enter(void* data, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {
    // Focus regained: discard keys held before focus was lost.
    auto* wl = static_cast<WaylandContext*>(data);
    if (wl->events) wl->events->KeyboardChanged();
}
static void keyboard_leave(void* data, wl_keyboard*, uint32_t, wl_surface*) {
    // The compositor may revoke focus (e.g. on lock); allow a later double-click to regrab.
    if (SetModelKeyboardGrab(static_cast<WaylandContext*>(data), false)
        != KeyboardSurfaceLifecycle::GrabResult::Changed)
        if (static_cast<WaylandContext*>(data)->events)
            static_cast<WaylandContext*>(data)->events->KeyboardChanged();
}
static void keyboard_key(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
    auto* wl = static_cast<WaylandContext*>(data);
    if (wl->events) wl->events->KeyboardKey(key, state == WL_KEYBOARD_KEY_STATE_PRESSED);
}
static void keyboard_modifiers(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
static void keyboard_repeat_info(void*, wl_keyboard*, int32_t, int32_t) {}
static const wl_keyboard_listener keyboard_listener = {
    keyboard_keymap, keyboard_enter, keyboard_leave, keyboard_key, keyboard_modifiers, keyboard_repeat_info
};

// --- Wayland Seat Handlers ---
static void seat_handle_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    WaylandContext* wl = static_cast<WaylandContext*>(data);
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !wl->pointer) {
        wl->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(wl->pointer, &pointer_listener, wl);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && wl->pointer) {
        wl_pointer_release(wl->pointer);
        wl->pointer = nullptr;
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !wl->keyboard) {
        wl->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(wl->keyboard, &keyboard_listener, wl);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && wl->keyboard) {
        if (SetModelKeyboardGrab(wl, false) != KeyboardSurfaceLifecycle::GrabResult::Changed)
            if (wl->events) wl->events->KeyboardChanged();
        wl_keyboard_destroy(wl->keyboard);
        wl->keyboard = nullptr;
    }
}
static void seat_handle_name(void *data, struct wl_seat *seat, const char *name) {}

static const struct wl_seat_listener seat_listener = {
    seat_handle_capabilities,
    seat_handle_name,
};

// --- Layer Surface Handlers ---
static void layer_surface_configure(void *data, struct zwlr_layer_surface_v1 *surface, uint32_t serial, uint32_t width, uint32_t height) {
    WaylandContext* wl = static_cast<WaylandContext*>(data);
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    if (wl->layer_surface != surface) return;
    wl->width = width;
    wl->height = height;
    
    // Dynamically resize EGL buffer when configure passes the new size
    if (wl->egl_window) {
        wl_egl_window_resize(wl->egl_window, wl->width, wl->height, 0, 0);
    }
    
    if (wl->keyboardSurface.OnConfigure()) {
        // A focus change while the new layer was awaiting its first configure
        // must only be committed after this configure has been acknowledged.
        ApplyLayerKeyboardInteractivity(wl);
        wl_surface_commit(wl->surface);
    }
    std::cout << "[Wayland] Configured! width: " << wl->width << ", height: " << wl->height << std::endl;
}
static void layer_surface_closed(void *data, struct zwlr_layer_surface_v1 *surface) {
    auto* wl = static_cast<WaylandContext*>(data);
    if (wl->layer_surface == surface && wl->events) wl->events->SurfaceClosed();
}
static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    layer_surface_configure,
    layer_surface_closed,
};

// --- Registry Handlers ---
static void output_handle_geometry(void* data, struct wl_output* wl_output, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, const char* make, const char* model, int32_t transform) {
    WaylandContext::OutputInfo* info = static_cast<WaylandContext::OutputInfo*>(data);
    info->x = x;
    info->y = y;
}
static void output_handle_mode(void* data, struct wl_output* wl_output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    WaylandContext::OutputInfo* info = static_cast<WaylandContext::OutputInfo*>(data);
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        info->width = width;
        info->height = height;
    }
}
static void output_handle_done(void* data, struct wl_output* wl_output) {}
static void output_handle_scale(void* data, struct wl_output* wl_output, int32_t factor) {}
static void output_handle_name(void* data, struct wl_output* wl_output, const char* name) {
    WaylandContext::OutputInfo* info = static_cast<WaylandContext::OutputInfo*>(data);
    strncpy(info->name, name, sizeof(info->name) - 1);
}
static void output_handle_description(void* data, struct wl_output* wl_output, const char* description) {}

static const struct wl_output_listener output_listener = {
    output_handle_geometry,
    output_handle_mode,
    output_handle_done,
    output_handle_scale,
    output_handle_name,
    output_handle_description
};

static void registry_handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    WaylandContext* wl = static_cast<WaylandContext*>(data);
    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        wl->compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 1));
    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        wl->shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        wl->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
        wl_seat_add_listener(wl->seat, &seat_listener, wl);
    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        wl->layer_shell = static_cast<zwlr_layer_shell_v1*>(wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 1));
    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        wl_output* out = static_cast<wl_output*>(wl_registry_bind(registry, name, &wl_output_interface,
            version < 4 ? version : 4));
        WaylandContext::OutputInfo* info = new WaylandContext::OutputInfo();
        info->owner = wl;
        info->id = name;
        info->output = out;
        info->x = 0; info->y = 0;
        info->width = 1920; info->height = 1080;
        memset(info->name, 0, sizeof(info->name));
        wl_output_add_listener(out, &output_listener, info);
        wl->outputs.push_back(info);
        if (!wl->current_output_id) wl->current_output_id = name;
    }
}
static void registry_handle_global_remove(void *data, struct wl_registry *, uint32_t name) {
    auto* wl = static_cast<WaylandContext*>(data);
    for (auto it = wl->outputs.begin(); it != wl->outputs.end(); ++it) {
        if ((*it)->id != name) continue;
        wl_output_destroy((*it)->output);
        delete *it;
        wl->outputs.erase(it);
        if (wl->current_output_id == name) wl->outputNeedsRebind = true;
        return;
    }
}

static const struct wl_registry_listener registry_listener = {
    registry_handle_global,
    registry_handle_global_remove,
};

bool SetupWaylandContext(WaylandContext* wl, int width, int height) {
    wl->keyboardSurface.Reset();
    wl->display = wl_display_connect(nullptr);
    if (!wl->display) {
        LAppPal::PrintLogLn("Failed to connect to Wayland display\n");
        return false;
    }

    wl->registry = wl_display_get_registry(wl->display);
    wl_registry_add_listener(wl->registry, &registry_listener, wl);
    if (wl_display_roundtrip(wl->display) < 0) return false;

    if (!wl->compositor || !wl->layer_shell || !wl->shm) {
        LAppPal::PrintLogLn("Missing required Wayland protocols (layer_shell, compositor or shm)\n");
        return false;
    }

    // Set up EGL
    wl->egl_display = eglGetDisplay((EGLNativeDisplayType)wl->display);
    if (wl->egl_display == EGL_NO_DISPLAY) {
        LAppPal::PrintLogLn("Failed to get EGL display");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(wl->egl_display, &major, &minor)) {
        LAppPal::PrintLogLn("Failed to initialize EGL");
        return false;
    }

    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };

    EGLint num_config;
    if (!eglChooseConfig(wl->egl_display, config_attribs, &wl->egl_config, 1, &num_config) || num_config == 0) {
        LAppPal::PrintLogLn("Failed to choose EGL config");
        return false;
    }

    if (!eglBindAPI(EGL_OPENGL_API)) {
        LAppPal::PrintLogLn("Failed to bind OpenGL API");
        return false;
    }

    const EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 2,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_NONE
    };

    wl->egl_context = eglCreateContext(wl->egl_display, wl->egl_config, EGL_NO_CONTEXT, context_attribs);
    if (wl->egl_context == EGL_NO_CONTEXT) {
        LAppPal::PrintLogLn("Failed to create EGL context");
        return false;
    }

    // Create Wayland & Layer Surface
    wl->surface = wl_compositor_create_surface(wl->compositor);
    
    // Choose OVERLAY or TOP for mascot. TOP gives desktop layer. OVERLAY gives full screen layer (above panels).
    wl->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        wl->layer_shell, wl->surface, nullptr, 
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "deskwaifu");
    
    zwlr_layer_surface_v1_add_listener(wl->layer_surface, &layer_surface_listener, wl);

    // Set interactive, anchor to all edges to get a full-screen transparent overlay
    zwlr_layer_surface_v1_set_size(wl->layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_anchor(wl->layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    ApplyLayerKeyboardInteractivity(wl);
    wl_surface_commit(wl->surface);
    if (wl_display_roundtrip(wl->display) < 0 || !wl->keyboardSurface.IsConfigured()) {
        LAppPal::PrintLogLn("[Wayland] Initial layer surface was not configured");
        return false;
    }
    if (wl->width <= 0) wl->width = width;
    if (wl->height <= 0) wl->height = height;

    // Now we have the configured width/height
    wl->egl_window = wl_egl_window_create(wl->surface, wl->width, wl->height);
    wl->egl_surface = eglCreateWindowSurface(wl->egl_display, wl->egl_config, (EGLNativeWindowType)wl->egl_window, nullptr);
    if (wl->egl_surface == EGL_NO_SURFACE) {
        LAppPal::PrintLogLn("Failed to create EGL surface");
        return false;
    }

    if (!MakeWaylandGpuContextCurrent(wl)) return false;
    eglSwapInterval(wl->egl_display, 1);

    return true;
}

KeyboardSurfaceLifecycle::GrabResult SetModelKeyboardGrab(WaylandContext* wl, bool grabbed) {
    using Result = KeyboardSurfaceLifecycle::GrabResult;
    if (!wl) return Result::Rejected;
    const Result result = wl->keyboardSurface.SetGrabbed(grabbed,
        wl->layer_surface != nullptr, wl->keyboard != nullptr);
    if (result != Result::Changed) return result;
    if (wl->layer_surface) {
        if (!wl->keyboardSurface.RequestInteractivityUpdate()) {
            ApplyLayerKeyboardInteractivity(wl);
            wl_surface_commit(wl->surface);
        }
    }
    if (!grabbed && wl->events) wl->events->KeyboardChanged();
    return result;
}

void CleanWaylandContext(WaylandContext* wl) {
    if (!wl) return;
    wl->keyboardSurface.Reset();
    if (wl->egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(wl->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (wl->egl_surface != EGL_NO_SURFACE) eglDestroySurface(wl->egl_display, wl->egl_surface);
        if (wl->egl_context != EGL_NO_CONTEXT) eglDestroyContext(wl->egl_display, wl->egl_context);
        eglTerminate(wl->egl_display);
        wl->egl_display = EGL_NO_DISPLAY;
        wl->egl_context = EGL_NO_CONTEXT;
        wl->egl_surface = EGL_NO_SURFACE;
    }
    if (wl->egl_window) wl_egl_window_destroy(wl->egl_window);
    if (wl->layer_surface) zwlr_layer_surface_v1_destroy(wl->layer_surface);
    if (wl->surface) wl_surface_destroy(wl->surface);
    if (wl->layer_shell) zwlr_layer_shell_v1_destroy(wl->layer_shell);
    if (wl->pointer) wl_pointer_destroy(wl->pointer);
    if (wl->keyboard) wl_keyboard_destroy(wl->keyboard);
    if (wl->seat) wl_seat_destroy(wl->seat);
    if (wl->shm) wl_shm_destroy(wl->shm);
    if (wl->compositor) wl_compositor_destroy(wl->compositor);
    if (wl->registry) wl_registry_destroy(wl->registry);
    for (auto* output : wl->outputs) {
        wl_output_destroy(output->output);
        delete output;
    }
    wl->outputs.clear();
    wl->current_output_id = 0;
    if (wl->display) wl_display_disconnect(wl->display);
    wl->display = nullptr;
}

bool MakeWaylandGpuContextCurrent(WaylandContext* wl)
{
    if (!wl || wl->egl_display == EGL_NO_DISPLAY || wl->egl_surface == EGL_NO_SURFACE ||
        wl->egl_context == EGL_NO_CONTEXT) return false;
    return eglMakeCurrent(wl->egl_display, wl->egl_surface, wl->egl_surface, wl->egl_context) == EGL_TRUE &&
        eglGetCurrentDisplay() == wl->egl_display && eglGetCurrentContext() == wl->egl_context &&
        eglGetCurrentSurface(EGL_DRAW) == wl->egl_surface;
}

std::vector<OutputSnapshot> WaylandBackend::Outputs() const
{
    std::vector<OutputSnapshot> snapshots;
    for (const auto* output : _wl.outputs)
        snapshots.push_back({output->id, output->name, output->x, output->y,
            output->width, output->height});
    return snapshots;
}

void WaylandBackend::ApplyMonitorGeometry(const std::vector<OutputSnapshot>& monitors)
{
    for (auto* output : _wl.outputs)
        for (const auto& monitor : monitors)
            if (monitor.name == output->name) {
                output->x = monitor.x;
                output->y = monitor.y;
                break;
            }
}

void WaylandBackend::ReconcileOutputs()
{
    if (_wl.protocolFailed) {
        if (_wl.events) _wl.events->SurfaceClosed();
        return;
    }
    if (!_wl.outputNeedsRebind) return;
    if (_wl.outputs.empty()) {
        if (_wl.events) _wl.events->SurfaceClosed();
        return;
    }
    SwitchOutput(_wl.outputs.front()->id);
}

bool WaylandBackend::SwitchOutput(OutputId id)
{
    if (!_wl.outputNeedsRebind && id == _wl.current_output_id) return true;
    WaylandContext::OutputInfo* chosen = nullptr;
    for (auto* output : _wl.outputs) if (output->id == id) chosen = output;
    if (!chosen || !_wl.compositor || !_wl.layer_shell || !_wl.display ||
        _wl.egl_context == EGL_NO_CONTEXT) return false;

    // Stage a new surface before discarding the old one. First configure
    // must be acknowledged before allocating its EGL window. On failure the
    // previously current EGL surface and stable OutputId remain usable.
    wl_surface* const previousSurface = _wl.surface;
    zwlr_layer_surface_v1* const previousLayer = _wl.layer_surface;
    wl_egl_window* const previousWindow = _wl.egl_window;
    EGLSurface const previousEgl = _wl.egl_surface;
    const int previousWidth = _wl.width, previousHeight = _wl.height;

    wl_surface* surface = wl_compositor_create_surface(_wl.compositor);
    if (!surface) return false;
    auto* layer = zwlr_layer_shell_v1_get_layer_surface(_wl.layer_shell, surface,
        chosen->output, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "deskwaifu");
    if (!layer) { wl_surface_destroy(surface); return false; }
    _wl.surface = surface;
    _wl.layer_surface = layer;
    _wl.egl_window = nullptr;
    _wl.width = _wl.height = 0;
    _wl.keyboardSurface.BeginSurface();
    zwlr_layer_surface_v1_add_listener(layer, &layer_surface_listener, &_wl);
    zwlr_layer_surface_v1_set_size(layer, 0, 0);
    zwlr_layer_surface_v1_set_anchor(layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    ApplyLayerKeyboardInteractivity(&_wl);
    wl_surface_commit(surface);
    const bool protocolOk = wl_display_roundtrip(_wl.display) >= 0;
    const bool configured = protocolOk && _wl.keyboardSurface.IsConfigured();
    if (configured && _wl.width <= 0) _wl.width = chosen->width;
    if (configured && _wl.height <= 0) _wl.height = chosen->height;
    wl_egl_window* window = configured ? wl_egl_window_create(surface, _wl.width, _wl.height) : nullptr;
    EGLSurface eglSurface = window ? eglCreateWindowSurface(_wl.egl_display, _wl.egl_config,
        reinterpret_cast<EGLNativeWindowType>(window), nullptr) : EGL_NO_SURFACE;
    bool bound = eglSurface != EGL_NO_SURFACE &&
        eglMakeCurrent(_wl.egl_display, eglSurface, eglSurface, _wl.egl_context) == EGL_TRUE;
    if (!bound) {
        if (eglSurface != EGL_NO_SURFACE) eglDestroySurface(_wl.egl_display, eglSurface);
        if (window) wl_egl_window_destroy(window);
        zwlr_layer_surface_v1_destroy(layer);
        wl_surface_destroy(surface);
        _wl.surface = previousSurface;
        _wl.layer_surface = previousLayer;
        _wl.egl_window = previousWindow;
        _wl.width = previousWidth;
        _wl.height = previousHeight;
        _wl.keyboardSurface.BeginSurface();
        _wl.keyboardSurface.OnConfigure();
        _wl.egl_surface = previousEgl;
        if (!protocolOk || previousEgl == EGL_NO_SURFACE || !MakeWaylandGpuContextCurrent(&_wl))
            _wl.protocolFailed = true;
        return false;
    }

    _wl.egl_window = window;
    _wl.egl_surface = eglSurface;
    if (previousEgl != EGL_NO_SURFACE) eglDestroySurface(_wl.egl_display, previousEgl);
    if (previousWindow) wl_egl_window_destroy(previousWindow);
    if (previousLayer) zwlr_layer_surface_v1_destroy(previousLayer);
    if (previousSurface) wl_surface_destroy(previousSurface);
    _wl.current_output_id = id;
    _wl.outputNeedsRebind = false;
    eglSwapInterval(_wl.egl_display, 1);
    LAppPal::PrintLogLn("[Wayland] Output switch configured and committed: %u", id);
    // eglMakeCurrent above already verified success for this exact context.
    return true;
}
