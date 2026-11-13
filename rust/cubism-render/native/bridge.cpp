#include "bridge.h"
#include <GL/glew.h>
#include <EGL/egl.h>
#include <CubismFramework.hpp>
#include <ICubismAllocator.hpp>
#include <Effect/CubismEyeBlink.hpp>
#include <Model/CubismMoc.hpp>
#include <Model/CubismModel.hpp>
#include <Id/CubismId.hpp>
#include <Math/CubismMatrix44.hpp>
#include <Motion/CubismExpressionMotion.hpp>
#include <Motion/CubismExpressionMotionManager.hpp>
#include <Motion/CubismMotion.hpp>
#include <Motion/CubismMotionManager.hpp>
#include <Motion/CubismMotionQueueEntry.hpp>
#include <Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp>
#include <Utils/CubismJson.hpp>
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
#include <unordered_map>
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
// Motion and expression objects are shared: the external handle and every
// playing queue entry hold a reference, so a manager keeps them alive until the
// entry finishes, is stopped, or the manager is dropped. Destroying the external
// handle only releases that handle; playback continues on the shared object.
struct Motion {
    ACubismMotion* motion = nullptr;
    ~Motion() { if (motion) ACubismMotion::Delete(motion); }
};
struct Expression {
    ACubismMotion* motion = nullptr;
    ~Expression() { if (motion) ACubismMotion::Delete(motion); }
};
using MotionRef = std::shared_ptr<Motion>;
using ExpressionRef = std::shared_ptr<Expression>;
struct Entry {
    CubismMotionQueueEntryHandle handle;
    MotionRef motion;
};
struct ExpressionEntry {
    CubismMotionQueueEntryHandle handle;
    ExpressionRef expression;
};
struct MotionManager {
    Model* model = nullptr;
    CubismMotionManager* manager = nullptr;
    std::unordered_map<uint64_t, Entry> entries;
    uint64_t next_token = 1;
    ~MotionManager() { if (manager) CSM_DELETE(manager); }
};
struct ExpressionManager {
    Model* model = nullptr;
    CubismExpressionMotionManager* manager = nullptr;
    std::unordered_map<uint64_t, ExpressionEntry> entries;
    uint64_t next_token = 1;
    ~ExpressionManager() { if (manager) CSM_DELETE(manager); }
};
struct EyeBlink {
    Model* model = nullptr;
    CubismEyeBlink* eye = nullptr;
    ~EyeBlink() { if (eye) CubismEyeBlink::Delete(eye); }
};
std::mutex mutex;
std::unordered_set<Model*> models;
std::unordered_set<Renderer*> renderers;
// The registry owns the external handle's reference; lookups also prove liveness.
std::unordered_map<Motion*, MotionRef> motions;
std::unordered_map<Expression*, ExpressionRef> expressions;
std::unordered_set<MotionManager*> motion_managers;
std::unordered_set<ExpressionManager*> expression_managers;
std::unordered_set<EyeBlink*> eye_blinks;
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
MotionRef motion(void* p) {
    const auto it = motions.find(static_cast<Motion*>(p));
    require(it != motions.end(), "invalid motion handle"); return it->second;
}
ExpressionRef expression(void* p) {
    const auto it = expressions.find(static_cast<Expression*>(p));
    require(it != expressions.end(), "invalid expression handle"); return it->second;
}
MotionManager* motion_manager(void* p) {
    auto* m = static_cast<MotionManager*>(p);
    require(motion_managers.count(m), "invalid motion manager handle"); return m;
}
ExpressionManager* expression_manager(void* p) {
    auto* m = static_cast<ExpressionManager*>(p);
    require(expression_managers.count(m), "invalid expression manager handle"); return m;
}
EyeBlink* eye_blink(void* p) {
    auto* e = static_cast<EyeBlink*>(p);
    require(eye_blinks.count(e), "invalid eye blink handle"); return e;
}
void index(Model* m, int i) {
    require(i >= 0 && i < m->model->GetParameterCount(), "parameter index out of bounds");
}
void seconds(float value, const char* message) {
    require(std::isfinite(value) && value >= 0.0f, message);
}
// Drop finished queue entries so their shared references are released. The SDK
// deletes the queue entry itself; only our token map needs pruning.
template<typename Manager>
void prune(Manager* m) {
    for (auto it = m->entries.begin(); it != m->entries.end();) {
        if (m->manager->GetCubismMotionQueueEntry(it->second.handle)) ++it;
        else it = m->entries.erase(it);
    }
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
void validate_bytes(const uint8_t* bytes, size_t size, const char* message) {
    require(bytes && size > 0 && size <= size_t(std::numeric_limits<csmSizeInt>::max()), message);
}
// The SDK parsers accept any well-formed JSON object, so an empty document would
// silently produce a motion/expression that never touches a parameter. Reject it
// here to keep "created" meaningful for callers.
void validate_json(const uint8_t* bytes, size_t size, const char* key, const char* message) {
    auto* json = Utils::CubismJson::Create(bytes, static_cast<csmSizeInt>(size));
    require(json, "JSON document could not be parsed");
    auto& root = json->GetRoot();
    const bool is_object = root.IsMap() && !root.IsError();
    // Prefer IsArray + GetVector: Map::GetSize() dereferences a lazily filled key
    // list, and a missing key would hand back the base allocator's vector.
    auto& items = is_object ? root[key] : root;
    const bool has_items = items.IsArray() && items.GetVector()->GetSize() > 0;
    Utils::CubismJson::Delete(json);
    require(is_object, "JSON root must be an object");
    require(has_items, message);
}
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
    current();
    require(models.empty() && renderers.empty() && motions.empty() && expressions.empty() &&
            motion_managers.empty() && expression_managers.empty() && eye_blinks.empty(),
            "live dependent handles prevent stop");
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
    validate_bytes(bytes, size, "invalid moc bytes");
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
    for (auto* g : motion_managers) require(g->model != m, "motion manager still owns model");
    for (auto* g : expression_managers) require(g->model != m, "expression manager still owns model");
    for (auto* e : eye_blinks) require(e->model != m, "eye blink still owns model");
    models.erase(m); delete m;
    API_END
}
int cr_model_update(void* p) {
    API_BEGIN current(); model(p)->model->Update(); API_END
}
int cr_model_load_parameters(void* p) {
    API_BEGIN current(); model(p)->model->LoadParameters(); API_END
}
int cr_model_save_parameters(void* p) {
    API_BEGIN current(); model(p)->model->SaveParameters(); API_END
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
int cr_motion_create(const uint8_t* bytes, size_t size, void** out) {
    API_BEGIN
    current(); require(out, "null output"); *out = nullptr;
    validate_bytes(bytes, size, "invalid motion3.json bytes");
    validate_json(bytes, size, "Curves", "motion3.json contains no curves");
    auto m = std::make_shared<Motion>();
    m->motion = CubismMotion::Create(bytes, static_cast<csmSizeInt>(size), nullptr, nullptr, true);
    require(m->motion, "motion3.json parse or consistency check failed");
    motions.emplace(m.get(), m); *out = m.get();
    API_END
}
int cr_motion_destroy(void* p) {
    API_BEGIN
    current(); motion(p);
    // Playing entries keep their own reference, so releasing the handle here is
    // safe and the motion object survives until every entry is pruned.
    motions.erase(static_cast<Motion*>(p));
    API_END
}
int cr_motion_set_fade_in(void* p, float seconds_value) {
    API_BEGIN
    current(); auto m = motion(p);
    seconds(seconds_value, "fade in seconds must be finite and non-negative");
    m->motion->SetFadeInTime(seconds_value);
    API_END
}
int cr_motion_set_fade_out(void* p, float seconds_value) {
    API_BEGIN
    current(); auto m = motion(p);
    seconds(seconds_value, "fade out seconds must be finite and non-negative");
    m->motion->SetFadeOutTime(seconds_value);
    API_END
}
int cr_motion_manager_create(void* p, void** out) {
    API_BEGIN
    current(); require(out, "null output"); *out = nullptr;
    auto g = std::make_unique<MotionManager>();
    g->model = model(p);
    g->manager = CSM_NEW CubismMotionManager();
    require(g->manager, "motion manager allocation failed");
    // `CubismMotionQueueManager::UpdateMotion` calls its event callback
    // unconditionally for every fired motion event, and the default callback is
    // null, so a motion with a `UserData` event would crash the process. The
    // desktop runtime has never acted on those events (the C++ never registers
    // one either), so the callback discards them instead of ending playback.
    g->manager->SetEventCallback(
        [](const CubismMotionQueueManager*, const csmString&, void*) {});
    motion_managers.insert(g.get()); *out = g.release();
    API_END
}
int cr_motion_manager_destroy(void* p) {
    API_BEGIN
    current(); auto* g = motion_manager(p);
    g->manager->StopAllMotions();
    g->entries.clear();
    motion_managers.erase(g); delete g;
    API_END
}
int cr_motion_manager_start(void* p, void* motion_handle, int priority, uint64_t* out_entry) {
    API_BEGIN
    current(); auto* g = motion_manager(p); auto m = motion(motion_handle);
    require(out_entry, "null motion entry output"); *out_entry = 0;
    require(models.count(g->model), "motion manager model is no longer alive");
    auto handle = g->manager->StartMotionPriority(m->motion, false, priority);
    require(handle != InvalidMotionQueueEntryHandleValue, "motion could not be started");
    *out_entry = g->next_token;
    g->entries.emplace(g->next_token++, Entry{handle, std::move(m)});
    API_END
}
int cr_motion_manager_update(void* p, float delta_seconds, int* out_updated) {
    API_BEGIN
    current(); auto* g = motion_manager(p); require(out_updated, "null update output");
    seconds(delta_seconds, "delta seconds must be finite and non-negative");
    *out_updated = g->manager->UpdateMotion(g->model->model, delta_seconds) ? 1 : 0;
    prune(g);
    API_END
}
int cr_motion_manager_stop(void* p) {
    API_BEGIN
    current(); auto* g = motion_manager(p);
    g->manager->StopAllMotions(); prune(g);
    API_END
}
int cr_motion_manager_is_finished(void* p, int* out_finished) {
    API_BEGIN
    current(); auto* g = motion_manager(p); require(out_finished, "null finished output");
    prune(g);
    *out_finished = g->manager->IsFinished() ? 1 : 0;
    API_END
}
int cr_motion_manager_entry_is_finished(void* p, uint64_t entry, int* out_finished) {
    API_BEGIN
    current(); auto* g = motion_manager(p); require(out_finished, "null finished output");
    require(entry != 0, "invalid motion entry token");
    prune(g);
    const auto it = g->entries.find(entry);
    if (it == g->entries.end()) { *out_finished = 1; return 0; }
    *out_finished = g->manager->IsFinished(it->second.handle) ? 1 : 0;
    API_END
}
int cr_expression_create(const uint8_t* bytes, size_t size, void** out) {
    API_BEGIN
    current(); require(out, "null output"); *out = nullptr;
    validate_bytes(bytes, size, "invalid exp3.json bytes");
    validate_json(bytes, size, "Parameters", "exp3.json contains no parameters");
    auto e = std::make_shared<Expression>();
    e->motion = CubismExpressionMotion::Create(bytes, static_cast<csmSizeInt>(size));
    require(e->motion, "exp3.json expression creation failed");
    expressions.emplace(e.get(), e); *out = e.get();
    API_END
}
int cr_expression_destroy(void* p) {
    API_BEGIN
    current(); expression(p);
    // Active managers hold their own reference; see cr_motion_destroy.
    expressions.erase(static_cast<Expression*>(p));
    API_END
}
int cr_expression_manager_create(void* p, void** out) {
    API_BEGIN
    current(); require(out, "null output"); *out = nullptr;
    auto g = std::make_unique<ExpressionManager>();
    g->model = model(p);
    g->manager = CSM_NEW CubismExpressionMotionManager();
    require(g->manager, "expression manager allocation failed");
    expression_managers.insert(g.get()); *out = g.release();
    API_END
}
int cr_expression_manager_destroy(void* p) {
    API_BEGIN
    current(); auto* g = expression_manager(p);
    g->manager->StopAllMotions();
    g->entries.clear();
    expression_managers.erase(g); delete g;
    API_END
}
int cr_expression_manager_start(void* p, void* expression_handle) {
    API_BEGIN
    current(); auto* g = expression_manager(p); auto e = expression(expression_handle);
    require(models.count(g->model), "expression manager model is no longer alive");
    auto handle = g->manager->StartMotion(e->motion, false);
    require(handle != InvalidMotionQueueEntryHandleValue, "expression could not be started");
    g->entries.emplace(g->next_token++, ExpressionEntry{handle, std::move(e)});
    API_END
}
int cr_expression_manager_update(void* p, float delta_seconds, int* out_updated) {
    API_BEGIN
    current(); auto* g = expression_manager(p); require(out_updated, "null update output");
    seconds(delta_seconds, "delta seconds must be finite and non-negative");
    *out_updated = g->manager->UpdateMotion(g->model->model, delta_seconds) ? 1 : 0;
    prune(g);
    API_END
}
int cr_eye_blink_create(void* p, const char* const* ids, int count, void** out) {
    API_BEGIN
    current(); require(out, "null output"); *out = nullptr;
    auto* m = model(p);
    require(ids && count > 0, "eye blink requires at least one parameter ID");
    csmVector<CubismIdHandle> handles;
    for (int i = 0; i < count; ++i) {
        require(ids[i] && *ids[i], "eye blink parameter ID required");
        CubismIdHandle found = nullptr;
        for (int j = 0; j < m->model->GetParameterCount(); ++j) {
            if (std::strcmp(m->model->GetParameterId(j)->GetString().GetRawString(), ids[i]) == 0) {
                found = m->model->GetParameterId(j); break;
            }
        }
        require(found, "eye blink parameter ID does not exist on model");
        handles.PushBack(found);
    }
    auto e = std::make_unique<EyeBlink>();
    e->model = m;
    e->eye = CubismEyeBlink::Create(nullptr);
    require(e->eye, "eye blink allocation failed");
    e->eye->SetParameterIds(handles);
    eye_blinks.insert(e.get()); *out = e.release();
    API_END
}
int cr_eye_blink_destroy(void* p) {
    API_BEGIN current(); auto* e = eye_blink(p); eye_blinks.erase(e); delete e; API_END
}
int cr_eye_blink_set_interval(void* p, float seconds_value) {
    API_BEGIN
    current(); auto* e = eye_blink(p);
    seconds(seconds_value, "blink interval must be finite and non-negative");
    e->eye->SetBlinkingInterval(seconds_value);
    API_END
}
int cr_eye_blink_set_settings(void* p, float closing, float closed, float opening) {
    API_BEGIN
    current(); auto* e = eye_blink(p);
    seconds(closing, "closing seconds must be finite and non-negative");
    seconds(closed, "closed seconds must be finite and non-negative");
    seconds(opening, "opening seconds must be finite and non-negative");
    require(closing > 0.0f && closed > 0.0f && opening > 0.0f, "blink phase durations must be positive");
    e->eye->SetBlinkingSettings(closing, closed, opening);
    API_END
}
int cr_eye_blink_update(void* p, void* model_handle, float delta_seconds) {
    API_BEGIN
    current(); auto* e = eye_blink(p); auto* m = model(model_handle);
    require(e->model == m, "eye blink belongs to a different model");
    seconds(delta_seconds, "delta seconds must be finite and non-negative");
    e->eye->UpdateParameters(m->model, delta_seconds);
    API_END
}
}
