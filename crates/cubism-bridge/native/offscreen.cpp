#include "internal.hpp"
#include "shaders.hpp"

#include <GL/glew.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <CubismFramework.hpp>
#include <ICubismAllocator.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace csm = Live2D::Cubism::Framework;
namespace {
std::atomic_flag runtime_active;

struct Slot {
    Slot() {
        if (runtime_active.test_and_set()) throw std::runtime_error("Cubism runtime already active");
    }
    ~Slot() { runtime_active.clear(); }
};

class Allocator final : public csm::ICubismAllocator {
public:
    void* Allocate(const csm::csmSizeType size) override { return std::malloc(size); }
    void Deallocate(void* pointer) override { std::free(pointer); }
    void* AllocateAligned(const csm::csmSizeType size, const csm::csmUint32 alignment) override {
        void* pointer = nullptr;
        return posix_memalign(&pointer, std::max<size_t>(sizeof(void*), alignment), size) == 0
            ? pointer : nullptr;
    }
    void DeallocateAligned(void* pointer) override { std::free(pointer); }
};

csm::csmByte* load_shader(const std::string path, csm::csmSizeInt* size) {
    const auto source = shader_source(path);
    *size = 0;
    if (source.empty()) return nullptr;
    auto bytes = std::make_unique<csm::csmByte[]>(source.size() + 1);
    std::memcpy(bytes.get(), source.data(), source.size());
    *size = static_cast<csm::csmSizeInt>(source.size());
    return bytes.release();
}

[[noreturn]] void egl_failure(const char* operation) {
    char message[128];
    std::snprintf(message, sizeof message, "%s failed (EGL 0x%x)", operation, eglGetError());
    throw std::runtime_error(message);
}

bool has_extension(const char* extensions, std::string_view name) {
    if (!extensions) return false;
    std::string_view list(extensions);
    while (!list.empty()) {
        const auto end = list.find(' ');
        if (list.substr(0, end) == name) return true;
        if (end == std::string_view::npos) break;
        list.remove_prefix(end + 1);
    }
    return false;
}

std::string normalize_pci(std::string address) {
    if (address.size() != 12 || address[4] != ':' || address[7] != ':' ||
        address[10] != '.' || address[11] < '0' || address[11] > '7') {
        return {};
    }
    for (size_t i = 0; i < 10; ++i) {
        if (i == 4 || i == 7) continue;
        auto& c = address[i];
        if (c >= 'A' && c <= 'F') c += 'a' - 'A';
        if ((c < '0' || c > '9') && (c < 'a' || c > 'f')) return {};
    }
    if (address[8] > '1') return {}; // PCI slot is five bits.
    return address;
}

struct Device {
    EGLDeviceEXT handle;
    std::string render_node;
    std::string pci;

    void visit(DwDeviceVisitor visitor, void* data) const {
        visitor(data, render_node.empty() ? nullptr : render_node.c_str(), pci.empty() ? nullptr : pci.c_str());
    }
};

Device describe_device(EGLDeviceEXT device) {
    Device description{device, {}, {}};
    const auto query_string = reinterpret_cast<PFNEGLQUERYDEVICESTRINGEXTPROC>(
        eglGetProcAddress("eglQueryDeviceStringEXT"));
    if (!query_string) return description;
    const char* extensions = query_string(device, EGL_EXTENSIONS);
    if (!extensions) (void)eglGetError();
    if (!has_extension(extensions, "EGL_EXT_device_drm_render_node")) return description;
    const char* node = query_string(device, EGL_DRM_RENDER_NODE_FILE_EXT);
    if (!node) {
        (void)eglGetError();
        return description;
    }
    namespace fs = std::filesystem;
    std::error_code error;
    const auto canonical = fs::canonical(node, error);
    description.render_node = error ? node : canonical.string();
    const auto sysfs = fs::path("/sys/class/drm") / fs::path(description.render_node).filename() / "device";
    const auto pci_path = fs::canonical(sysfs, error);
    if (!error) description.pci = normalize_pci(pci_path.filename().string());
    return description;
}

std::vector<EGLDeviceEXT> enumerate_devices() {
    const auto extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!has_extension(extensions, "EGL_EXT_device_enumeration") &&
        !has_extension(extensions, "EGL_EXT_device_base")) {
        if (!extensions) (void)eglGetError();
        throw std::runtime_error("EGL device enumeration unavailable");
    }
    const auto query_devices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(eglGetProcAddress("eglQueryDevicesEXT"));
    if (!query_devices) throw std::runtime_error("EGL device enumeration unavailable");
    EGLint count = 0;
    if (!query_devices(0, nullptr, &count)) egl_failure("eglQueryDevicesEXT");
    if (count <= 0) return {};
    std::vector<EGLDeviceEXT> handles(count);
    if (!query_devices(count, handles.data(), &count)) egl_failure("eglQueryDevicesEXT");
    handles.resize(count);
    return handles;
}

EGLDeviceEXT select_device(bool by_pci, const std::string& selector) {
    std::string key;
    if (by_pci) {
        key = normalize_pci(selector);
        if (key.empty()) throw std::invalid_argument("invalid PCI address (expected dddd:bb:ss.f)");
    } else {
        const std::filesystem::path path(selector);
        if (!path.is_absolute()) throw std::invalid_argument("DRM render node path must be absolute");
        std::error_code error;
        const auto canonical = std::filesystem::canonical(path, error);
        key = error ? selector : canonical.string();
    }

    for (auto handle : enumerate_devices()) {
        const auto device = describe_device(handle);
        if ((by_pci ? device.pci : device.render_node) == key) return handle;
    }
    throw std::runtime_error("requested EGL device is unavailable: " + selector);
}

EGLDeviceEXT display_device(EGLDisplay display) {
    const auto query_attrib = reinterpret_cast<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(
        eglGetProcAddress("eglQueryDisplayAttribEXT"));
    EGLAttrib handle = 0;
    if (!query_attrib) return EGL_NO_DEVICE_EXT;
    if (!query_attrib(display, EGL_DEVICE_EXT, &handle)) {
        (void)eglGetError();
        return EGL_NO_DEVICE_EXT;
    }
    return reinterpret_cast<EGLDeviceEXT>(handle);
}

struct Egl {
    EGLenum old_api = eglQueryAPI();
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    bool initialized = false;
    bool bound_api = false;
    std::optional<Device> active_device;

    void initialize(void* native_display, void* native_window, EGLDeviceEXT requested) {
        if (eglGetCurrentContext() != EGL_NO_CONTEXT) {
            throw std::runtime_error("Cubism requires a thread without a current EGL context");
        }
        const auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
            eglGetProcAddress("eglGetPlatformDisplayEXT"));
        if (!get_platform_display) throw std::runtime_error("EGL platform display unavailable");
        const bool windowed = native_window != nullptr;
        if (requested != EGL_NO_DEVICE_EXT) {
            display = get_platform_display(EGL_PLATFORM_DEVICE_EXT, requested, nullptr);
        } else {
            display = get_platform_display(windowed ? EGL_PLATFORM_WAYLAND_KHR : EGL_PLATFORM_SURFACELESS_MESA,
                                           native_display, nullptr);
        }
        if (display == EGL_NO_DISPLAY || !eglInitialize(display, nullptr, nullptr)) {
            if (requested != EGL_NO_DEVICE_EXT) egl_failure("explicit-device eglInitialize");
            egl_failure(windowed ? "Wayland eglInitialize" : "surfaceless eglInitialize");
        }
        initialized = true;
        const auto active = display_device(display);
        if (requested != EGL_NO_DEVICE_EXT && active != requested) {
            throw std::runtime_error("EGL display did not report the requested device");
        }
        if (active != EGL_NO_DEVICE_EXT) active_device = describe_device(active);
        if (!eglBindAPI(EGL_OPENGL_API)) egl_failure("eglBindAPI(OpenGL)");
        bound_api = true;
        const EGLint config_attributes[] = {
            EGL_SURFACE_TYPE, windowed ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE,
        };
        EGLConfig config;
        EGLint count;
        if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || count == 0) egl_failure("eglChooseConfig");
        const EGLint surface_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        if (windowed) {
            const auto create_window_surface = reinterpret_cast<PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC>(
                eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT"));
            if (!create_window_surface) throw std::runtime_error("EGL platform window surface unavailable");
            surface = create_window_surface(display, config, native_window, nullptr);
        } else {
            surface = eglCreatePbufferSurface(display, config, surface_attributes);
        }
        if (surface == EGL_NO_SURFACE) egl_failure(windowed ? "eglCreatePlatformWindowSurfaceEXT" : "eglCreatePbufferSurface");
        const EGLint context_attributes[] = {
            EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
            EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT, EGL_NONE,
        };
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
        if (context == EGL_NO_CONTEXT) egl_failure("eglCreateContext(OpenGL compatibility)");
        require_current();
        if (windowed && !eglSwapInterval(display, 0)) egl_failure("eglSwapInterval");
        glewExperimental = GL_TRUE;
        if (const auto result = glewInit(); result != GLEW_OK) {
            throw std::runtime_error(reinterpret_cast<const char*>(glewGetErrorString(result)));
        }
        while (glGetError() != GL_NO_ERROR) {} // GLEW may leave probe errors.
    }

    void require_current() const {
        if (eglGetCurrentContext() == context) return;
        if (!eglMakeCurrent(display, surface, surface, context)) egl_failure("eglMakeCurrent");
    }

    ~Egl() {
        if (initialized) {
            if (context != EGL_NO_CONTEXT && eglGetCurrentContext() == context) {
                eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            }
            if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
            if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
            eglTerminate(display);
        }
        if (bound_api) eglBindAPI(old_api);
    }
};
} // namespace

thread_local char error_text[1024];

struct DwRuntime {
    Slot slot;
    Egl egl;
    Allocator allocator;
    csm::CubismFramework::Option options{};
    bool started = false;
    GLuint framebuffer = 0;
    GLuint texture = 0;
    GLuint mask_framebuffer = 0;
    GLuint mask_texture = 0;
    bool windowed = false;
    bool window_ready = false;
    int32_t width = 0;
    int32_t height = 0;
    std::string renderer;

    void initialize(int32_t frame_width, int32_t frame_height, void* display = nullptr,
                    void* window = nullptr, EGLDeviceEXT requested = EGL_NO_DEVICE_EXT) {
        width = frame_width;
        height = frame_height;
        if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
            throw std::invalid_argument("frame dimensions must be in 1..=8192");
        }
        windowed = window != nullptr;
        egl.initialize(display, window, requested);
        renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
        options.LogFunction = [](const char* message) { std::fprintf(stderr, "%s\n", message); };
        options.LoggingLevel = csm::CubismFramework::Option::LogLevel_Error;
        options.LoadFileFunction = load_shader;
        options.ReleaseBytesFunction = [](csm::csmByte* bytes) { delete[] bytes; };
        started = csm::CubismFramework::StartUp(&allocator, &options);
        if (!started) throw std::runtime_error("Cubism Framework startup failed");
        csm::CubismFramework::Initialize();
        if (!csm::CubismFramework::IsInitialized()) throw std::runtime_error("Cubism Framework initialization failed");

        create_target(framebuffer, texture, width, height);
        if (windowed) {
            create_target(mask_framebuffer, mask_texture, (width + 7) / 8, (height + 7) / 8);
        }
    }

    static void create_target(GLuint& framebuffer, GLuint& texture, int width, int height) {
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE || glGetError() != GL_NO_ERROR) {
            throw std::runtime_error("cannot initialize RGBA8 framebuffer");
        }
    }

    ~DwRuntime() {
        if (started) {
            if (csm::CubismFramework::IsInitialized()) csm::CubismFramework::Dispose();
            csm::CubismFramework::CleanUp();
        }
        if (mask_framebuffer) glDeleteFramebuffers(1, &mask_framebuffer);
        if (mask_texture) glDeleteTextures(1, &mask_texture);
        if (framebuffer) glDeleteFramebuffers(1, &framebuffer);
        if (texture) glDeleteTextures(1, &texture);
    }
};

extern "C" const char* dw_error() { return error_text; }

extern "C" int32_t dw_create(int32_t width, int32_t height, DwRuntime** out) {
    if (!out) return 1;
    *out = nullptr;
    return checked([&] {
        auto runtime = std::make_unique<DwRuntime>();
        runtime->initialize(width, height);
        *out = runtime.release();
    });
}

extern "C" int32_t dw_devices(DwDeviceVisitor visitor, void* data) {
    return checked([&] {
        if (!visitor) throw std::invalid_argument("missing EGL device visitor");
        for (auto handle : enumerate_devices()) describe_device(handle).visit(visitor, data);
    });
}

extern "C" void dw_active_device(const DwRuntime* runtime, DwDeviceVisitor visitor, void* data) {
    if (runtime && visitor && runtime->egl.active_device) runtime->egl.active_device->visit(visitor, data);
}

extern "C" int32_t dw_create_on_device(int32_t width, int32_t height, int32_t by_pci,
                                       const uint8_t* selector, size_t length, DwRuntime** out) {
    if (!out) return 1;
    *out = nullptr;
    return checked([&] {
        auto runtime = std::make_unique<DwRuntime>();
        const auto key = input_string(selector, length);
        const auto selected = select_device(by_pci != 0, key);
        try {
            // Mesa 25.0 can crash during eglInitialize when hardware selection conflicts
            // with forced software rendering. Reject the conflict without changing the environment.
            if (const char* value = std::getenv("LIBGL_ALWAYS_SOFTWARE")) {
                std::string flag(value);
                for (auto& c : flag) {
                    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                }
                if (flag == "1" || flag == "true" || flag == "yes" || flag == "on" ||
                    flag == "t" || flag == "y") {
                    throw std::runtime_error("LIBGL_ALWAYS_SOFTWARE conflicts with explicit hardware selection");
                }
            }
            runtime->initialize(width, height, nullptr, nullptr, selected);
        } catch (const std::exception& error) {
            throw std::runtime_error("cannot use EGL device " + key + ": " + error.what());
        }
        *out = runtime.release();
    });
}

extern "C" int32_t dw_create_wayland(int32_t width, int32_t height, void* display,
                                     void* window, DwRuntime** out) {
    if (!out) return 1;
    *out = nullptr;
    return checked([&] {
        if (!display || !window) throw std::invalid_argument("missing native Wayland display or window");
        auto runtime = std::make_unique<DwRuntime>();
        runtime->initialize(width, height, display, window);
        *out = runtime.release();
    });
}

extern "C" const char* dw_renderer(const DwRuntime* runtime) { return runtime->renderer.c_str(); }
extern "C" void dw_destroy(DwRuntime* runtime) { delete runtime; }

int32_t viewport_width(const DwRuntime& runtime) { return runtime.width; }
int32_t viewport_height(const DwRuntime& runtime) { return runtime.height; }

void begin_frame(DwRuntime& runtime) {
    runtime.window_ready = false;
    runtime.egl.require_current();
    glBindFramebuffer(GL_FRAMEBUFFER, runtime.framebuffer);
    glViewport(0, 0, runtime.width, runtime.height);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
}

namespace {
void check_gl_frame() {
    if (const auto error = glGetError(); error != GL_NO_ERROR) {
        char message[96];
        std::snprintf(message, sizeof message, "Cubism frame failed (OpenGL 0x%x)", error);
        throw std::runtime_error(message);
    }
}

void read_rgba(GLuint source, int width, int height, uint8_t* pixels, size_t length) {
    const size_t stride = static_cast<size_t>(width) * 4;
    if (!pixels || length != stride * height) throw std::invalid_argument("invalid RGBA buffer size");
    glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
    glReadBuffer(source ? GL_COLOR_ATTACHMENT0 : GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    check_gl_frame();
    for (int row = 0; row < height / 2; ++row) {
        auto top = pixels + row * stride;
        std::swap_ranges(top, top + stride, pixels + (height - row - 1) * stride);
    }
}

void blit(const DwRuntime& runtime, GLuint target, int width, int height) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, runtime.framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glDrawBuffer(target ? GL_COLOR_ATTACHMENT0 : GL_BACK);
    glBlitFramebuffer(0, 0, runtime.width, runtime.height, 0, 0, width, height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    check_gl_frame();
}
} // namespace

void read_frame(const DwRuntime& runtime, uint8_t* pixels, size_t length) {
    read_rgba(runtime.framebuffer, runtime.width, runtime.height, pixels, length);
}

void require_window(const DwRuntime& runtime) {
    if (!runtime.windowed) throw std::invalid_argument("operation requires a Wayland EGL surface");
}

void stage_window(DwRuntime& runtime) {
    blit(runtime, 0, runtime.width, runtime.height);
    runtime.window_ready = true;
}

extern "C" int32_t dw_read_window(DwRuntime* runtime, int32_t sampled, uint8_t* pixels, size_t length) {
    return checked([&] {
        if (!runtime || !runtime->window_ready) throw std::logic_error("no staged window frame");
        runtime->egl.require_current();
        if (sampled) {
            const int width = (runtime->width + 7) / 8;
            const int height = (runtime->height + 7) / 8;
            blit(*runtime, runtime->mask_framebuffer, width, height);
            read_rgba(runtime->mask_framebuffer, width, height, pixels, length);
        } else {
            read_rgba(0, runtime->width, runtime->height, pixels, length);
        }
    });
}

extern "C" int32_t dw_present_window(DwRuntime* runtime) {
    return checked([&] {
        if (!runtime || !runtime->window_ready) throw std::logic_error("no staged window frame");
        runtime->egl.require_current();
        runtime->window_ready = false;
        if (!eglSwapBuffers(runtime->egl.display, runtime->egl.surface)) egl_failure("eglSwapBuffers");
    });
}
