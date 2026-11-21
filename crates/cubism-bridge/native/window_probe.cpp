// Wayland transport used only by the window benchmark example.
#include "internal.hpp"
// wayland-scanner can emit the protocol argument name `namespace` in C headers.
#include <wayland-client.h>
#define namespace layer_namespace
#include "layer-shell.h"
#undef namespace
#include <wayland-egl.h>

#include <cerrno>
#include <cstring>
#include <memory>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {
struct Buffer {
    wl_buffer* proxy = nullptr;
    void* pixels = MAP_FAILED;
    size_t size = 0;
    bool busy = false;

    void create(wl_shm* shm, int width, int height) {
        size = static_cast<size_t>(width) * height * 4;
        const int fd = memfd_create("cubism-window", MFD_CLOEXEC);
        if (fd < 0) throw std::runtime_error("memfd_create failed");
        if (ftruncate(fd, size) == 0) {
            pixels = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        }
        if (pixels == MAP_FAILED) {
            close(fd);
            throw std::runtime_error("cannot map wl_shm buffer");
        }
        auto* pool = wl_shm_create_pool(shm, fd, size);
        close(fd);
        if (!pool) throw std::runtime_error("cannot create wl_shm pool");
        proxy = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        if (!proxy) throw std::runtime_error("cannot create wl_shm buffer");
        static const wl_buffer_listener listener = {
            [](void* data, wl_buffer*) { static_cast<Buffer*>(data)->busy = false; },
        };
        wl_buffer_add_listener(proxy, &listener, this);
    }

    ~Buffer() {
        if (proxy) wl_buffer_destroy(proxy);
        if (pixels != MAP_FAILED) munmap(pixels, size);
    }
};

struct Layer {
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_shm* shm = nullptr;
    wl_compositor* compositor = nullptr;
    zwlr_layer_shell_v1* shell = nullptr;
    wl_surface* surface = nullptr;
    zwlr_layer_surface_v1* layer = nullptr;
    wl_egl_window* window = nullptr;
    wl_callback* callback = nullptr;
    std::unique_ptr<Buffer[]> buffers;
    bool configured = false;
    bool closed = false;
    int width = 0;
    int height = 0;

    void initialize(int w, int h, bool egl) {
        width = w;
        height = h;
        display = wl_display_connect(nullptr);
        if (!display) throw std::runtime_error("cannot connect to Wayland");
        registry = wl_display_get_registry(display);
        static const wl_registry_listener globals = {
            [](void* data, wl_registry* r, uint32_t name, const char* interface, uint32_t) {
                auto& p = *static_cast<Layer*>(data);
                if (!std::strcmp(interface, wl_compositor_interface.name)) {
                    p.compositor = static_cast<wl_compositor*>(wl_registry_bind(r, name, &wl_compositor_interface, 1));
                } else if (!std::strcmp(interface, wl_shm_interface.name)) {
                    p.shm = static_cast<wl_shm*>(wl_registry_bind(r, name, &wl_shm_interface, 1));
                } else if (!std::strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
                    p.shell = static_cast<zwlr_layer_shell_v1*>(wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, 1));
                }
            },
            [](void*, wl_registry*, uint32_t) {},
        };
        wl_registry_add_listener(registry, &globals, this);
        if (wl_display_roundtrip(display) < 0 || !compositor || !shell || (!egl && !shm)) {
            throw std::runtime_error("Wayland compositor, layer-shell or wl_shm unavailable");
        }
        surface = wl_compositor_create_surface(compositor);
        layer = zwlr_layer_shell_v1_get_layer_surface(shell, surface, nullptr,
            ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "cubism-window-benchmark");
        static const zwlr_layer_surface_v1_listener configure = {
            [](void* data, zwlr_layer_surface_v1* target, uint32_t serial, uint32_t, uint32_t) {
                zwlr_layer_surface_v1_ack_configure(target, serial);
                static_cast<Layer*>(data)->configured = true;
            },
            [](void* data, zwlr_layer_surface_v1*) { static_cast<Layer*>(data)->closed = true; },
        };
        zwlr_layer_surface_v1_add_listener(layer, &configure, this);
        zwlr_layer_surface_v1_set_anchor(layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
        zwlr_layer_surface_v1_set_size(layer, width, height);
        zwlr_layer_surface_v1_set_exclusive_zone(layer, -1);
        zwlr_layer_surface_v1_set_keyboard_interactivity(layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
        wl_surface_commit(surface);
        if (wl_display_roundtrip(display) < 0 || !configured || closed) {
            throw std::runtime_error("layer-shell configure failed");
        }
        if (egl) {
            window = wl_egl_window_create(surface, width, height);
            if (!window) throw std::runtime_error("wl_egl_window_create failed");
        } else {
            buffers = std::make_unique<Buffer[]>(3);
            for (int i = 0; i < 3; ++i) buffers[i].create(shm, width, height);
        }
    }

    Buffer* free_buffer() const {
        if (buffers) {
            for (int i = 0; i < 3; ++i) if (!buffers[i].busy) return &buffers[i];
        }
        return nullptr;
    }

    void prepare(const int32_t* rects, size_t count) {
        if (callback || (!window && !free_buffer())) throw std::logic_error("Wayland frame is not ready");
        auto* region = wl_compositor_create_region(compositor);
        for (size_t i = 0; i < count; ++i, rects += 4) {
            wl_region_add(region, rects[0], rects[1], rects[2], rects[3]);
        }
        wl_surface_set_input_region(surface, region);
        wl_region_destroy(region);
        callback = wl_surface_frame(surface);
        static const wl_callback_listener listener = {
            [](void* data, wl_callback* event, uint32_t) {
                wl_callback_destroy(event);
                static_cast<Layer*>(data)->callback = nullptr;
            },
        };
        wl_callback_add_listener(callback, &listener, this);
        // The caller's swap or shm attach and commit applies this region and callback.
    }

    void commit_shm(const uint8_t* rgba) {
        auto* buffer = free_buffer();
        if (!buffer || !callback) throw std::logic_error("wl_shm frame not prepared");
        auto* argb = static_cast<uint32_t*>(buffer->pixels);
        for (size_t i = 0; i < buffer->size / 4; ++i, rgba += 4) {
            argb[i] = static_cast<uint32_t>(rgba[3]) << 24 | rgba[0] << 16 | rgba[1] << 8 | rgba[2];
        }
        buffer->busy = true;
        wl_surface_attach(surface, buffer->proxy, 0, 0);
        wl_surface_damage(surface, 0, 0, width, height);
        wl_surface_commit(surface);
    }

    void wait(int ipc_fd, int timeout_ms, int32_t& flags) {
        // Pair each prepare_read with read_events or cancel_read, including errors.
        while (wl_display_prepare_read(display) != 0) {
            if (wl_display_dispatch_pending(display) < 0) throw std::runtime_error("Wayland disconnected");
        }
        short events = POLLIN;
        if (wl_display_flush(display) < 0) {
            if (errno != EAGAIN) {
                wl_display_cancel_read(display);
                throw std::runtime_error("Wayland flush failed");
            }
            events |= POLLOUT;
        }
        if (!callback && (window || free_buffer())) timeout_ms = 0;
        pollfd fds[] = {{wl_display_get_fd(display), events, 0}, {ipc_fd, POLLIN, 0}};
        const int ready = poll(fds, 2, timeout_ms);
        if (ready > 0 && (fds[0].revents & (POLLIN | POLLERR | POLLHUP))) {
            if (wl_display_read_events(display) < 0) throw std::runtime_error("Wayland disconnected");
        } else {
            wl_display_cancel_read(display);
            if (ready < 0 && errno != EINTR) throw std::runtime_error("Wayland poll failed");
        }
        if (wl_display_dispatch_pending(display) < 0 || closed) throw std::runtime_error("Wayland layer closed");
        flags = (!callback && (window || free_buffer()) ? 1 : 0) | (fds[1].revents & POLLIN ? 2 : 0);
    }

    ~Layer() {
        buffers.reset();
        if (callback) wl_callback_destroy(callback);
        if (window) wl_egl_window_destroy(window);
        if (layer) zwlr_layer_surface_v1_destroy(layer);
        if (surface) wl_surface_destroy(surface);
        if (shell) zwlr_layer_shell_v1_destroy(shell);
        if (shm) wl_shm_destroy(shm);
        if (compositor) wl_compositor_destroy(compositor);
        if (registry) wl_registry_destroy(registry);
        if (display) wl_display_disconnect(display);
    }
};
} // namespace

extern "C" int32_t dw_probe_open(int32_t w, int32_t h, int32_t egl, void** out,
                                 void** display, void** window) {
    return checked([&] {
        auto layer = std::make_unique<Layer>();
        layer->initialize(w, h, egl != 0);
        *display = layer->display;
        *window = layer->window;
        *out = layer.release();
    });
}
extern "C" int32_t dw_probe_wait(void* layer, int ipc, int ms, int32_t* flags) {
    return checked([&] { static_cast<Layer*>(layer)->wait(ipc, ms, *flags); });
}
extern "C" int32_t dw_probe_prepare(void* layer, const int32_t* rects, size_t count) {
    return checked([&] { static_cast<Layer*>(layer)->prepare(rects, count); });
}
extern "C" int32_t dw_probe_commit_shm(void* layer, const uint8_t* rgba) {
    return checked([&] { static_cast<Layer*>(layer)->commit_shm(rgba); });
}
extern "C" void dw_probe_close(void* layer) { delete static_cast<Layer*>(layer); }
