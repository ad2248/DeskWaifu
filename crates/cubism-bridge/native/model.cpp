#include "internal.hpp"

#include <GL/glew.h>
#include <CubismDefaultParameterId.hpp>
#include <CubismModelSettingJson.hpp>
#include <Effect/CubismEyeBlink.hpp>
#include <Effect/CubismLook.hpp>
#include <Id/CubismIdManager.hpp>
#include <Effect/CubismPose.hpp>
#include <Model/CubismUserModel.hpp>
#include <Motion/CubismMotion.hpp>
#include <Utils/CubismJson.hpp>
#include <Physics/CubismPhysics.hpp>
#include <Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp>

#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string_view>
#include <utility>
#include <vector>

namespace csm = Live2D::Cubism::Framework;
namespace fs = std::filesystem;
using Renderer = csm::Rendering::CubismRenderer_OpenGLES2;

using Motion = std::unique_ptr<csm::ACubismMotion, decltype(&csm::ACubismMotion::Delete)>;

namespace {
constexpr int tap_priority = 2;
constexpr int force_priority = 3;

bool nonnegative(float seconds) { return std::isfinite(seconds) && seconds >= 0.0f; }
bool optional_duration(float seconds) { return seconds == -1.0f || nonnegative(seconds); }

enum class Blend { Add, Multiply, Overwrite };
struct ExpressionParameter {
    int index;
    Blend blend;
    float value;
};

struct ExpressionToggle {
    fs::path file;
    std::vector<ExpressionParameter> parameters;
    bool active = false;
    float fade = 0.0f;
    float weight = 0.0f;
    double remaining = -1.0;

    void advance_weight(float dt) {
        const float step = fade > 0.0f ? dt / fade : 1.0f;
        weight = active ? std::min(1.0f, weight + step) : std::max(0.0f, weight - step);
    }

    void update(float dt) {
        if (active && remaining >= 0.0) {
            if (dt >= remaining) {
                // Split at expiration so a single large frame doesn't skip the fade-in.
                const float elapsed = static_cast<float>(remaining);
                if (elapsed > 0.0f) advance_weight(elapsed);
                active = false;
                remaining = -1.0;
                dt -= elapsed;
            } else {
                remaining -= dt;
            }
        }
        advance_weight(dt);
    }
};

fs::path relative_asset_path(const std::string& name) {
    const fs::path relative(name);
    if (relative.empty() || relative.is_absolute() ||
        std::any_of(relative.begin(), relative.end(), [](const fs::path& part) { return part == ".."; })) {
        throw std::runtime_error("expected a relative model asset path");
    }
    return relative.lexically_normal();
}

std::vector<csm::csmByte> read_asset(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    const auto length = stream.tellg();
    if (length <= 0 || length > 128 * 1024 * 1024) {
        throw std::runtime_error("cannot read asset: " + path.string());
    }
    std::vector<csm::csmByte> bytes(static_cast<size_t>(length));
    if (!stream.seekg(0).read(reinterpret_cast<char*>(bytes.data()), length)) {
        throw std::runtime_error("incomplete asset: " + path.string());
    }
    return bytes;
}

struct Texture {
    GLuint id = 0;
    Texture() { glGenTextures(1, &id); }
    ~Texture() { if (id) glDeleteTextures(1, &id); }
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
};
} // namespace

struct DwModel final : csm::CubismUserModel {
    DwRuntime& runtime;
    fs::path resource_dir;
    std::unique_ptr<csm::CubismModelSettingJson> settings;
    std::vector<std::unique_ptr<Texture>> textures;
    csm::csmVector<csm::CubismIdHandle> blink_ids, lip_ids;
    std::optional<float> lip_opening;
    std::optional<std::pair<float, float>> look_target;

    int skin_index = 0;
    int next_expression = 0;
    std::optional<std::vector<int>> skin_parameters;
    std::optional<std::mt19937> tap_rng;
    std::vector<ExpressionToggle> expression_toggles;
    float expression_timeout = -1.0f;
    std::optional<double> expression_age;

    explicit DwModel(DwRuntime& owner) : runtime(owner) {}
    ~DwModel() override { DeleteRenderer(); }

    std::vector<csm::csmByte> asset(const char* name) const {
        fs::path relative(name);
        if (relative.empty() || relative.is_absolute() ||
            std::any_of(relative.begin(), relative.end(), [](const fs::path& part) { return part == ".."; })) {
            throw std::runtime_error("expected a relative model asset path");
        }
        return read_asset(resource_dir / relative);
    }

    int parameter_index(csm::CubismIdHandle id) const {
        // GetParameterIndex creates a synthetic parameter for an unknown ID.
        for (int i = 0; i < _model->GetParameterCount(); ++i) {
            if (_model->GetParameterId(i) == id) return i;
        }
        return -1;
    }

    void initialize_controls() {
        for (int i = 0; i < settings->GetEyeBlinkParameterCount(); ++i) {
            const auto id = settings->GetEyeBlinkParameterId(i);
            if (parameter_index(id) >= 0) blink_ids.PushBack(id);
        }
        for (int i = 0; i < settings->GetLipSyncParameterCount(); ++i) {
            const auto id = settings->GetLipSyncParameterId(i);
            if (parameter_index(id) >= 0) lip_ids.PushBack(id);
        }
        if (blink_ids.GetSize() > 0) {
            _eyeBlink = csm::CubismEyeBlink::Create();
            _eyeBlink->SetParameterIds(blink_ids);
        }

        using namespace csm::DefaultParameterId;
        const auto ids = csm::CubismFramework::GetIdManager();
        csm::csmVector<csm::CubismLook::LookParameterData> parameters;
        const auto add = [&](const char* name, float x, float y, float xy = 0.0f) {
            const auto id = ids->GetId(name);
            if (parameter_index(id) >= 0) parameters.PushBack({id, x, y, xy});
        };
        add(ParamAngleX, 30.0f, 0.0f);
        add(ParamAngleY, 0.0f, 30.0f);
        add(ParamAngleZ, 0.0f, 0.0f, -30.0f);
        add(ParamBodyAngleX, 10.0f, 0.0f);
        add(ParamBodyAngleY, 0.0f, 10.0f);
        add(ParamBodyAngleZ, 0.0f, 0.0f, -10.0f);
        add(ParamEyeBallX, 1.0f, 0.0f);
        add(ParamEyeBallY, 0.0f, 1.0f);
        if (parameters.GetSize() > 0) {
            _look = csm::CubismLook::Create();
            _look->SetParameters(parameters);
        }
    }

    void initialize(const fs::path& model_json) {
        resource_dir = model_json.parent_path();
        const auto json = read_asset(model_json);
        settings = std::make_unique<csm::CubismModelSettingJson>(json.data(), json.size());
        if (!settings->GetJsonPointer()) throw std::runtime_error("invalid model3.json: " + model_json.string());
        const auto moc = asset(settings->GetModelFileName());
        LoadModel(moc.data(), moc.size(), true);
        if (!_model || !_modelMatrix) throw std::runtime_error("invalid Cubism moc3");

        if (const auto file = settings->GetPhysicsFileName(); *file) {
            const auto bytes = asset(file);
            LoadPhysics(bytes.data(), bytes.size());
            if (!_physics) throw std::runtime_error("invalid Cubism physics file");
        }
        if (const auto file = settings->GetPoseFileName(); *file) {
            const auto bytes = asset(file);
            LoadPose(bytes.data(), bytes.size());
            if (!_pose) throw std::runtime_error("invalid Cubism pose file");
        }
        initialize_controls();

        csm::csmMap<csm::csmString, csm::csmFloat32> layout;
        settings->GetLayoutMap(layout);
        _modelMatrix->SetupFromLayout(layout);
        CreateRenderer(viewport_width(runtime), viewport_height(runtime));
        const auto renderer = GetRenderer<Renderer>();
        if (!renderer) throw std::runtime_error("cannot create Cubism renderer");
        if (settings->GetTextureCount() <= 0) throw std::runtime_error("model has no textures");
        for (int index = 0; index < settings->GetTextureCount(); ++index) {
            auto bytes = asset(settings->GetTextureFileName(index));
            int width = 0, height = 0, channels = 0;
            std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
                stbi_load_from_memory(bytes.data(), bytes.size(), &width, &height, &channels, 4), stbi_image_free);
            if (!pixels) throw std::runtime_error("invalid model PNG texture: " + std::string(settings->GetTextureFileName(index)));
            auto texture = std::make_unique<Texture>();
            glBindTexture(GL_TEXTURE_2D, texture->id);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.get());
            // Cubism 5-r.5 sets GL_LINEAR_MIPMAP_LINEAR when binding an atlas.
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            if (glGetError() != GL_NO_ERROR) throw std::runtime_error("cannot upload Cubism texture");
            renderer->BindTexture(index, texture->id);
            textures.push_back(std::move(texture));
        }
        // PNG inputs use straight alpha. Cubism blends to a premultiplied render target.
        renderer->IsPremultipliedAlpha(false);
    }

    Motion load_motion(const char* group, int32_t index) {
        if (index < 0 || index >= settings->GetMotionCount(group)) {
            throw std::runtime_error("unknown Cubism motion group/index: " + std::string(group) + "/" + std::to_string(index));
        }
        const auto bytes = asset(settings->GetMotionFileName(group, index));
        Motion motion(
            LoadMotion(bytes.data(), bytes.size(), nullptr, nullptr, nullptr, settings.get(), group, index, true),
            csm::ACubismMotion::Delete);
        if (!motion) throw std::runtime_error("invalid Cubism motion");
        static_cast<csm::CubismMotion*>(motion.get())->SetEffectIds(blink_ids, lip_ids);
        return motion;
    }

    void start_motion(Motion motion, int priority) {
        _motionManager->StartMotionPriority(motion.get(), true, priority);
        motion.release(); // The motion queue owns the motion after enqueue.
    }

    ExpressionToggle load_toggle(const fs::path& file) const {
        const auto bytes = read_asset(resource_dir / file);
        // The SDK parser needs a comma or newline after numbers, even in compact JSON.
        std::string normalized;
        normalized.reserve(bytes.size());
        bool quoted = false, escaped = false;
        for (const char c : bytes) {
            if (!quoted && (c == '}' || c == ']')) normalized += '\n';
            normalized += !quoted && std::isspace(static_cast<unsigned char>(c)) ? '\n' : c;
            if (escaped) escaped = false;
            else if (quoted && c == '\\') escaped = true;
            else if (c == '"') quoted = !quoted;
        }
        using Json = csm::Utils::CubismJson;
        std::unique_ptr<Json, decltype(&Json::Delete)> json(
            Json::Create(reinterpret_cast<const csm::csmByte*>(normalized.data()), normalized.size()), Json::Delete);
        ExpressionToggle toggle{file, {}};
        if (json) {
            auto& parameters = json->GetRoot()["Parameters"];
            if (parameters.IsArray()) {
                for (int i = 0; i < parameters.GetSize(); ++i) {
                    auto& entry = parameters[i];
                    auto& id = entry["Id"];
                    auto& value = entry["Value"];
                    auto& blend_value = entry["Blend"];
                    if (!id.IsString() || !value.IsFloat() || !std::isfinite(value.ToFloat())) continue;
                    const int index = parameter_index(csm::CubismFramework::GetIdManager()->GetId(id.GetRawString()));
                    if (index < 0) continue;
                    const std::string_view mode = blend_value.IsString() ? blend_value.GetRawString() : "";
                    Blend blend;
                    if (mode == "Multiply") blend = Blend::Multiply;
                    else if (mode == "Overwrite") blend = Blend::Overwrite;
                    else if (mode.empty() || mode == "Add") blend = Blend::Add;
                    else continue;
                    toggle.parameters.push_back({index, blend, value.ToFloat()});
                }
            }
        }
        if (toggle.parameters.empty()) throw std::runtime_error("expression has no applicable parameters: " + file.string());
        return toggle;
    }

    bool toggle_expression(const fs::path& file, float fade, float duration) {
        auto it = std::find_if(expression_toggles.begin(), expression_toggles.end(),
                               [&](const auto& toggle) { return toggle.file == file; });
        if (it == expression_toggles.end()) {
            expression_toggles.push_back(load_toggle(file));
            it = std::prev(expression_toggles.end());
        }
        it->active = !it->active;
        it->fade = fade;
        it->remaining = it->active ? duration : -1.0;
        return it->active;
    }

    void apply_expression_toggles(float dt) {
        for (auto& toggle : expression_toggles) {
            toggle.update(dt);
            if (toggle.weight <= 0.0f) continue;
            for (const auto& parameter : toggle.parameters) {
                const auto [index, blend, value] = parameter;
                switch (blend) {
                    case Blend::Add: _model->AddParameterValue(index, value, toggle.weight); break;
                    case Blend::Multiply: _model->MultiplyParameterValue(index, value, toggle.weight); break;
                    case Blend::Overwrite: _model->SetParameterValue(index, value, toggle.weight); break;
                }
            }
        }
    }

    void expression_at(int index) {
        const auto bytes = asset(settings->GetExpressionFileName(index));
        Motion motion(LoadExpression(bytes.data(), bytes.size(), settings->GetExpressionName(index)),
                      csm::ACubismMotion::Delete);
        if (!motion) throw std::runtime_error("invalid Cubism expression");
        _expressionManager->StartMotion(motion.get(), true);
        motion.release(); // The expression queue owns the motion after enqueue.
        expression_age = 0.0;
    }

    void expression(const std::string& name) {
        for (int i = 0; i < settings->GetExpressionCount(); ++i) {
            if (name != settings->GetExpressionName(i)) continue;
            expression_at(i);
            return;
        }
        throw std::runtime_error("unknown Cubism expression: " + name);
    }

    void cycle_expression() {
        const auto count = settings->GetExpressionCount();
        if (count == 0) return;
        expression_at(next_expression);
        // The legacy "random expression" command actually cycles the list.
        next_expression = (next_expression + 1) % count;
    }

    std::vector<int> collect_skin_parameters() {
        std::vector<int> parameters;
        const auto count = settings->GetMotionGroupCount();
        if (count <= 1) return parameters;
        for (int group_index = 0; group_index < count; ++group_index) {
            const auto group = settings->GetMotionGroupName(group_index);
            for (int i = 0; i < settings->GetMotionCount(group); ++i) {
                std::vector<csm::csmByte> bytes;
                try {
                    bytes = asset(settings->GetMotionFileName(group, i));
                } catch (const std::runtime_error&) {
                    // An unavailable motion in another group need not block the switch.
                    continue;
                }
                using Json = csm::Utils::CubismJson;
                std::unique_ptr<Json, decltype(&Json::Delete)> document(
                    Json::Create(bytes.data(), bytes.size()), Json::Delete);
                if (!document) continue;
                auto& curves = document->GetRoot()["Curves"];
                if (!curves.IsArray()) continue;
                for (int curve = 0; curve < curves.GetSize(); ++curve) {
                    auto& target = curves[curve]["Target"];
                    auto& id = curves[curve]["Id"];
                    if (!target.IsString() || std::string_view(target.GetRawString()) != "Parameter" ||
                        !id.IsString()) continue;
                    const int index = parameter_index(csm::CubismFramework::GetIdManager()->GetId(id.GetRawString()));
                    if (index >= 0 && std::find(parameters.begin(), parameters.end(), index) == parameters.end()) {
                        parameters.push_back(index);
                    }
                }
            }
        }
        return parameters;
    }

    void switch_skin() {
        const auto count = settings->GetMotionGroupCount();
        if (count == 0) {
            cycle_expression();
            return;
        }

        // Legacy starts from group zero, advances, then plays the new group's first motion.
        const auto next = (skin_index + 1) % count;
        const auto group = settings->GetMotionGroupName(next);
        if (settings->GetMotionCount(group) == 0) {
            skin_index = next;
            return;
        }
        auto motion = load_motion(group, 0);
        if (!skin_parameters) skin_parameters = collect_skin_parameters();

        // Clear old curves before resetting their parameters, so a fading motion can't
        // write the previous skin's values back over the reset on the next frame.
        _motionManager->StopAllMotions();
        _model->LoadParameters();
        for (const int index : *skin_parameters) {
            _model->SetParameterValue(index, _model->GetParameterDefaultValue(index));
        }
        _model->SaveParameters();
        start_motion(std::move(motion), force_priority);
        skin_index = next;
    }

    csm::CubismMatrix44 render_projection() const {
        const auto width = static_cast<float>(viewport_width(runtime));
        const auto height = static_cast<float>(viewport_height(runtime));
        const auto aspect = width / height;
        const auto fit = std::min(2.0f / _model->GetCanvasHeight(), 2.0f * aspect / _model->GetCanvasWidth());
        csm::CubismMatrix44 mvp;
        mvp.Scale(fit / aspect, fit);
        mvp.TranslateRelative(_modelMatrix->GetTranslateX() / _modelMatrix->GetScaleX(),
                              _modelMatrix->GetTranslateY() / _modelMatrix->GetScaleY());
        return mvp;
    }

    DwTapHit hit_area(float x, float y) const {
        if (_model->GetModelOpacity() < 1.0f) return DW_TAP_MISS;
        const auto count = settings->GetHitAreasCount();
        // Legacy splits the viewport before applying model layout for this fallback.
        if (count == 0) return y < 0.5f ? DW_TAP_HEAD : DW_TAP_BODY;

        auto mvp = render_projection();
        const float local_x = mvp.InvertTransformX(2.0f * x - 1.0f);
        const float local_y = mvp.InvertTransformY(1.0f - 2.0f * y);
        DwTapHit hit = DW_TAP_MISS;
        for (int area = 0; area < count; ++area) {
            const std::string_view name = settings->GetHitAreaName(area);
            if (name != "Head" && name != "Body") continue;
            const auto index = _model->GetDrawableIndex(settings->GetHitAreaId(area));
            if (index < 0) continue;
            const auto count = _model->GetDrawableVertexCount(index);
            if (count <= 0) continue;
            const auto vertices = _model->GetDrawableVertices(index);
            // Cubism hit areas use an axis-aligned bounding box of the deformed vertices.
            float left = vertices[0], right = left, bottom = vertices[1], top = bottom;
            for (int i = 1; i < count; ++i) {
                left = std::min(left, vertices[2 * i]);
                right = std::max(right, vertices[2 * i]);
                bottom = std::min(bottom, vertices[2 * i + 1]);
                top = std::max(top, vertices[2 * i + 1]);
            }
            if (local_x >= left && local_x <= right && local_y >= bottom && local_y <= top) {
                if (name == "Head") return DW_TAP_HEAD;
                hit = DW_TAP_BODY;
            }
        }
        return hit;
    }

    DwTapHit tap(float x, float y) {
        look_target.reset();
        const auto hit = hit_area(x, y);
        Motion body_motion(nullptr, csm::ACubismMotion::Delete);
        if (hit == DW_TAP_BODY) {
            const auto count = settings->GetMotionCount("TapBody");
            if (count > 0 && tap_priority > _motionManager->GetCurrentPriority() &&
                tap_priority > _motionManager->GetReservePriority()) {
                if (!tap_rng) tap_rng.emplace(std::random_device{}());
                const auto index = std::uniform_int_distribution<int>(0, count - 1)(*tap_rng);
                body_motion = load_motion("TapBody", index);
            }
        }
        if (hit != DW_TAP_MISS) cycle_expression();
        if (body_motion) start_motion(std::move(body_motion), tap_priority);
        return hit;
    }

    void draw(float dt) {
        _model->LoadParameters();
        const bool motion_updated = _motionManager->UpdateMotion(_model, dt);
        // Persist only motion parameters. Late effects must not accumulate across frames.
        _model->SaveParameters();
        if (_eyeBlink && !motion_updated) _eyeBlink->UpdateParameters(_model, dt);
        if (expression_age) {
            *expression_age += dt;
            if (expression_timeout >= 0.0f && *expression_age > expression_timeout) {
                _expressionManager->StopAllMotions();
                expression_age.reset();
            }
        }
        _expressionManager->UpdateMotion(_model, dt);
        if (_look && look_target) _look->UpdateParameters(_model, look_target->first, look_target->second);
        if (_physics) _physics->Evaluate(_model, dt);
        if (_pose) _pose->UpdateParameters(_model, dt);
        // Keep toggles outside the saved motion baseline and the ordinary expression queue.
        apply_expression_toggles(dt);
        // An external mouth opening is an explicit override, as in the legacy update.
        if (lip_opening) {
            for (int i = 0; i < lip_ids.GetSize(); ++i) _model->SetParameterValue(lip_ids[i], *lip_opening);
        }
        _model->Update();

        auto mvp = render_projection();
        auto renderer = GetRenderer<Renderer>();
        renderer->SetMvpMatrix(&mvp);
        renderer->DrawModel();
    }
};

extern "C" int32_t dw_load(DwRuntime* runtime, const uint8_t* path, size_t length, DwModel** out) {
    if (!out) return 1;
    *out = nullptr;
    return checked([&] {
        if (!runtime) throw std::invalid_argument("missing Cubism runtime");
        begin_frame(*runtime);
        auto model = std::make_unique<DwModel>(*runtime);
        model->initialize(input_string(path, length));
        *out = model.release();
    });
}

extern "C" int32_t dw_uses_masking(const DwModel* model) {
    return model->GetModel()->IsUsingMasking() || model->GetModel()->IsUsingMaskingForOffscreen();
}

extern "C" int32_t dw_metadata_count(DwModel* model, int32_t expressions) {
    return expressions ? model->settings->GetExpressionCount() : model->settings->GetMotionGroupCount();
}

extern "C" int32_t dw_metadata_entry(DwModel* model, int32_t expressions, int32_t index,
                                       const char** name, int32_t* motions) {
    return checked([&] {
        if (!model || !name || !motions || index < 0 || index >= dw_metadata_count(model, expressions)) {
            throw std::invalid_argument("invalid metadata index");
        }
        *name = expressions ? model->settings->GetExpressionName(index) : model->settings->GetMotionGroupName(index);
        *motions = expressions ? 0 : model->settings->GetMotionCount(*name);
    });
}

extern "C" int32_t dw_expression(DwModel* model, const uint8_t* name, size_t length) {
    return checked([&] {
        if (!model) throw std::invalid_argument("missing Cubism model");
        model->expression(input_string(name, length));
    });
}

extern "C" int32_t dw_set_expression_timeout(DwModel* model, float timeout) {
    return checked([&] {
        if (!model || !optional_duration(timeout)) throw std::invalid_argument("invalid expression timeout");
        model->expression_timeout = timeout;
    });
}

extern "C" int32_t dw_toggle_expression(DwModel* model, const uint8_t* file, size_t length,
                                         float fade, float duration, int32_t* active) {
    return checked([&] {
        if (!model || !active || !nonnegative(fade) || !optional_duration(duration)) {
            throw std::invalid_argument("invalid expression toggle arguments");
        }
        *active = model->toggle_expression(relative_asset_path(input_string(file, length)), fade, duration);
    });
}

extern "C" int32_t dw_expression_toggle_active(const DwModel* model, const uint8_t* file, size_t length,
                                                int32_t* active) {
    return checked([&] {
        if (!model || !active) throw std::invalid_argument("invalid expression toggle query");
        const auto path = relative_asset_path(input_string(file, length));
        *active = std::any_of(model->expression_toggles.begin(), model->expression_toggles.end(),
                              [&](const auto& toggle) { return toggle.active && toggle.file == path; });
    });
}

extern "C" void dw_reset_expression_toggles(DwModel* model) {
    if (model) model->expression_toggles.clear();
}

extern "C" int32_t dw_motion(DwModel* model, const uint8_t* group, size_t length, int32_t index) {
    return checked([&] {
        if (!model) throw std::invalid_argument("missing Cubism model");
        const auto name = input_string(group, length);
        model->start_motion(model->load_motion(name.c_str(), index), force_priority);
    });
}

extern "C" int32_t dw_switch_skin(DwModel* model) {
    return checked([&] {
        if (!model) throw std::invalid_argument("missing Cubism model");
        model->switch_skin();
    });
}

extern "C" int32_t dw_tap(DwModel* model, float x, float y, int32_t* hit) {
    return checked([&] {
        if (!model || !hit || !std::isfinite(x) || x < 0.0f || x > 1.0f ||
            !std::isfinite(y) || y < 0.0f || y > 1.0f) {
            throw std::invalid_argument("invalid tap coordinates or model");
        }
        *hit = model->tap(x, y);
    });
}

extern "C" int32_t dw_set_lip_sync(DwModel* model, int32_t active, float opening) {
    return checked([&] {
        if (!model || (active != 0 && active != 1) ||
            (active && (!std::isfinite(opening) || opening < 0.0f || opening > 1.0f))) {
            throw std::invalid_argument("invalid lip sync control");
        }
        if (active) model->lip_opening = opening;
        else model->lip_opening.reset();
    });
}

extern "C" int32_t dw_set_look(DwModel* model, int32_t active, float x, float y) {
    return checked([&] {
        if (!model || (active != 0 && active != 1) ||
            (active && (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1.0f || std::abs(y) > 1.0f))) {
            throw std::invalid_argument("invalid look control");
        }
        if (active) model->look_target.emplace(x, y);
        else model->look_target.reset();
    });
}

extern "C" int32_t dw_frame(DwModel* model, float dt, uint8_t* pixels, size_t length) {
    return checked([&] {
        if (!model || !std::isfinite(dt) || dt < 0.0f || dt > 1.0f) throw std::invalid_argument("invalid frame step");
        begin_frame(model->runtime);
        model->draw(dt);
        read_frame(model->runtime, pixels, length);
    });
}

extern "C" int32_t dw_draw_window(DwModel* model, float dt) {
    return checked([&] {
        if (!model || !std::isfinite(dt) || dt < 0.0f || dt > 1.0f) throw std::invalid_argument("invalid frame step");
        require_window(model->runtime);
        begin_frame(model->runtime);
        model->draw(dt);
        stage_window(model->runtime);
    });
}

extern "C" void dw_model_destroy(DwModel* model) { delete model; }
