#include "bridge.h"
#include <GL/glew.h>
#include <EGL/egl.h>
#include <CubismFramework.hpp>
#include <ICubismAllocator.hpp>
#include <Model/CubismMoc.hpp>
#include <Model/CubismModel.hpp>
#include <Id/CubismId.hpp>
#include <Math/CubismMatrix44.hpp>
#include <Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_set>

namespace {
using namespace Live2D::Cubism::Framework;
using Rendering::CubismRenderer_OpenGLES2;
// CubismShader_OpenGLES2::GenerateShaders aliases one GL program across the
// Normal/Add/Mult shader sets, but its ReleaseShaderProgram deletes the handle
// stored in every set. Framework::Dispose consequently calls glDeleteProgram on
// names it already deleted, which raises GL_INVALID_VALUE and leaves it pending
// for the caller's next glGetError. Skip the stale deletes instead of clearing
// the error afterwards: glIsProgram reports a dead name without raising, so
// disposal stays genuinely error-free. Only stale names are skipped; deleting a
// live program still goes to the real driver entry point.
PFNGLDELETEPROGRAMPROC real_delete_program = nullptr;
void delete_live_program(GLuint program) {
    if (glIsProgram(program)) real_delete_program(program);
}
struct DeleteLiveProgramsOnly final {
    DeleteLiveProgramsOnly() : real_(__glewDeleteProgram) {
        real_delete_program = real_; __glewDeleteProgram = delete_live_program;
    }
    ~DeleteLiveProgramsOnly() { __glewDeleteProgram = real_; real_delete_program = nullptr; }
    DeleteLiveProgramsOnly(const DeleteLiveProgramsOnly&) = delete;
    DeleteLiveProgramsOnly& operator=(const DeleteLiveProgramsOnly&) = delete;
private:
    PFNGLDELETEPROGRAMPROC real_;
};
struct Allocator final : ICubismAllocator {
    void* Allocate(csmSizeType n) override { return std::malloc(n); }
    void Deallocate(void* p) override { std::free(p); }
    void* AllocateAligned(csmSizeType n, csmUint32 a) override {
        void* p = nullptr;
        return posix_memalign(&p, std::max<size_t>(a, sizeof(void*)), n) == 0 ? p : nullptr;
    }
    void DeallocateAligned(void* p) override { std::free(p); }
} allocator;
struct Model {
    CubismMoc* moc = nullptr;
    CubismModel* model = nullptr;
    ~Model() { if (model) moc->DeleteModel(model); if (moc) CubismMoc::Delete(moc); }
};
struct Renderer {
    Model* model;
    CubismRenderer_OpenGLES2* renderer = nullptr;
    ~Renderer() { if (renderer) Rendering::CubismRenderer::Delete(renderer); }
};
std::mutex mutex;
std::unordered_set<Model*> models;
std::unordered_set<Renderer*> renderers;
CubismFramework::Option option{}; // Framework retains this pointer until CleanUp.
bool active = false;
EGLContext context = EGL_NO_CONTEXT;
std::thread::id thread;
std::filesystem::path shader_dir;
bool shader_failed = false;
thread_local std::string error;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void current() {
    require(active, "Framework is not started");
    require(thread == std::this_thread::get_id() && context == eglGetCurrentContext(),
            "original EGL context must be current on its original thread");
}
Model* model(void* p) {
    auto* m = static_cast<Model*>(p);
    require(models.count(m), "invalid model handle"); return m;
}
Renderer* renderer(void* p) {
    auto* r = static_cast<Renderer*>(p);
    require(renderers.count(r), "invalid renderer handle"); return r;
}
void index(Model* m, int i) {
    require(i >= 0 && i < m->model->GetParameterCount(), "parameter index out of bounds");
}
csmByte* load(const std::string path, csmSizeInt* size) noexcept {
    try {
        // SDK desktop paths start with FrameworkShaders/. Only the basename is used.
        const auto file = shader_dir / std::filesystem::path(path).filename();
        std::ifstream stream(file, std::ios::binary | std::ios::ate);
        require(stream.good(), "cannot open external shader");
        auto n = stream.tellg();
        require(n > 0 && n < std::numeric_limits<csmSizeInt>::max(), "invalid shader length");
        auto bytes = std::make_unique<csmByte[]>(static_cast<size_t>(n) + 1);
        stream.seekg(0);
        require(bool(stream.read(reinterpret_cast<char*>(bytes.get()), n)), "cannot read shader");
        bytes[static_cast<size_t>(n)] = 0;
        *size = static_cast<csmSizeInt>(n);
        return bytes.release();
    } catch (...) { shader_failed = true; if (size) *size = 0; return nullptr; }
}
void release(csmByte* bytes) noexcept { delete[] bytes; }
#define API_BEGIN try { std::lock_guard<std::mutex> lock(mutex);
#define API_END return 0; } catch (const std::exception& e) { error = e.what(); return -1; } catch (...) { error = "unknown C++ exception"; return -1; }
}
extern "C" {
const char* cr_error() { return error.c_str(); }
int cr_start(const char* directory) {
    API_BEGIN
    require(!active, "Framework already owned by another session");
    require(directory && *directory, "shader directory required");
    require(eglGetCurrentContext() != EGL_NO_CONTEXT, "current EGL OpenGL context required");
    shader_dir = directory;
    require(std::filesystem::is_regular_file(shader_dir / "VertShaderSrc.vert"), "not a Standard shader directory");
    glewExperimental = GL_TRUE;
    require(glewInit() == GLEW_OK, "EGL GLEW initialization failed");
    option = {};
    option.LoggingLevel = CubismFramework::Option::LogLevel_Off;
    option.LoadFileFunction = load;
    option.ReleaseBytesFunction = release;
    require(CubismFramework::StartUp(&allocator, &option), "Framework startup failed");
    try { CubismFramework::Initialize(); }
    catch (...) { CubismFramework::CleanUp(); throw; }
    context = eglGetCurrentContext(); thread = std::this_thread::get_id(); active = true;
    API_END
}
int cr_stop() {
    API_BEGIN
    current(); require(models.empty() && renderers.empty(), "live dependent handles prevent stop");
    {
        DeleteLiveProgramsOnly guard;
        CubismFramework::Dispose(); CubismFramework::CleanUp();
    }
    active = false; context = EGL_NO_CONTEXT;
    API_END
}
int cr_model_create(const uint8_t* bytes, size_t size, void** out) {
    API_BEGIN
    current(); require(out, "null output"); *out = nullptr;
    require(bytes && size > 0 && size <= size_t(std::numeric_limits<csmSizeInt>::max()), "invalid moc bytes");
    auto m = std::make_unique<Model>();
    m->moc = CubismMoc::Create(bytes, static_cast<csmSizeInt>(size), true);
    require(m->moc, "moc consistency/version validation failed");
    m->model = m->moc->CreateModel(); require(m->model, "model creation failed");
    models.insert(m.get()); *out = m.release();
    API_END
}
int cr_model_destroy(void* p) {
    API_BEGIN
    current(); auto* m = model(p);
    for (auto* r : renderers) require(r->model != m, "renderer still owns model");
    models.erase(m); delete m;
    API_END
}
int cr_model_update(void* p) {
    API_BEGIN current(); model(p)->model->Update(); API_END
}
int cr_model_canvas(void* p, float* w, float* h) {
    API_BEGIN current(); require(w && h, "null output"); auto* m = model(p)->model;
    *w = m->GetCanvasWidth(); *h = m->GetCanvasHeight(); API_END
}
int cr_parameter_count(void* p, int* count) {
    API_BEGIN current(); require(count, "null output"); *count = model(p)->model->GetParameterCount(); API_END
}
int cr_parameter_get(void* p, int i, const char** id, float* value, float* min, float* max, float* initial) {
    API_BEGIN current(); auto* m = model(p); index(m, i);
    require(id && value && min && max && initial, "null parameter output");
    *id = m->model->GetParameterId(i)->GetString().GetRawString();
    *value = m->model->GetParameterValue(i); *min = m->model->GetParameterMinimumValue(i);
    *max = m->model->GetParameterMaximumValue(i); *initial = m->model->GetParameterDefaultValue(i);
    API_END
}
int cr_parameter_set(void* p, int i, float value) {
    API_BEGIN current(); auto* m = model(p); index(m, i);
    require(std::isfinite(value), "parameter must be finite");
    m->model->SetParameterValue(i, value); API_END
}
int cr_renderer_create(void* p, int w, int h, void** out) {
    API_BEGIN current(); require(out, "null output"); *out = nullptr;
    require(w > 0 && h > 0, "invalid target size");
    auto r = std::make_unique<Renderer>(); r->model = model(p);
    r->renderer = static_cast<CubismRenderer_OpenGLES2*>(Rendering::CubismRenderer::Create(w, h));
    require(r->renderer, "renderer allocation failed");
    r->renderer->Initialize(r->model->model); r->renderer->IsPremultipliedAlpha(false);
    r->renderer->SetRenderTargetSize(w, h);
    renderers.insert(r.get()); *out = r.release(); API_END
}
int cr_renderer_destroy(void* p) {
    API_BEGIN current(); auto* r = renderer(p); renderers.erase(r); delete r; API_END
}
int cr_renderer_bind_texture(void* p, int slot, uint32_t texture) {
    API_BEGIN current(); auto* r = renderer(p);
    require(slot >= 0 && glIsTexture(texture), "invalid texture slot or GL texture");
    // Only actual drawable texture slots may be bound.
    bool found = false;
    for (int i = 0; i < r->model->model->GetDrawableCount(); ++i) found |= r->model->model->GetDrawableTextureIndex(i) == slot;
    require(found, "texture slot not used by model");
    r->renderer->BindTexture(slot, texture); API_END
}
int cr_renderer_resize(void* p, int w, int h) {
    API_BEGIN current(); require(w > 0 && h > 0, "invalid target size");
    renderer(p)->renderer->SetRenderTargetSize(w, h); API_END
}
int cr_renderer_draw(void* p, const float* values) {
    API_BEGIN current(); auto* r = renderer(p); require(values, "null matrix");
    for (int i = 0; i < 16; ++i) require(std::isfinite(values[i]), "matrix must be finite");
    for (int i = 0; i < r->model->model->GetDrawableCount(); ++i)
        require(r->renderer->GetBindedTextures().IsExist(r->model->model->GetDrawableTextureIndex(i)), "unbound drawable texture");
    CubismMatrix44 matrix; matrix.SetMatrix(const_cast<float*>(values));
    r->renderer->SetMvpMatrix(&matrix); shader_failed = false; r->renderer->DrawModel();
    require(!shader_failed, "external shader loading failed"); API_END
}
}
