#include "AppCommandHandler.hpp"
#include "LAppDelegate.hpp"
#include "LAppLive2DManager.hpp"
#include "LAppModel.hpp"
#include "LAppDefine.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <limits>

using namespace Csm;

// ─── Minimal JSON helpers (no external dependency) ───────────────────────────
// We only need to produce JSON output and parse simple {"command":"...", ...} input.

static std::string JsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); i++)
    {
        char c = s[i];
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:   out += c; break;
        }
    }
    return out;
}

static std::string Reply(const AppResult& result)
{
    return result.ok ? "{\"ok\":true}"
        : "{\"ok\":false,\"error\":\"" + JsonEscape(result.error) + "\"}";
}

// Very simple key extraction from a flat JSON object. Returns empty string if not found.
static std::string JsonGetString(const std::string& json, const std::string& key)
{
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return "";

    // Skip whitespace
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;

    if (pos >= json.size()) return "";

    if (json[pos] == '"')
    {
        // String value
        pos++;
        std::string val;
        while (pos < json.size() && json[pos] != '"')
        {
            if (json[pos] == '\\' && pos + 1 < json.size())
            {
                pos++;
            }
            val += json[pos];
            pos++;
        }
        return val;
    }
    else
    {
        // Number or other literal
        std::string val;
        while (pos < json.size() && json[pos] != ',' && json[pos] != '}' && json[pos] != ' ')
        {
            val += json[pos];
            pos++;
        }
        return val;
    }
}

static float JsonGetFloat(const std::string& json, const std::string& key, float defaultVal)
{
    std::string val = JsonGetString(json, key);
    if (val.empty()) return defaultVal;
    char* end = nullptr;
    errno = 0;
    float parsed = strtof(val.c_str(), &end);
    return errno || end == val.c_str() || *end != '\0'
        ? std::numeric_limits<float>::quiet_NaN() : parsed;
}

static int JsonGetInt(const std::string& json, const std::string& key, int defaultVal)
{
    std::string val = JsonGetString(json, key);
    if (val.empty()) return defaultVal;
    return atoi(val.c_str());
}

// ─── Command handlers ─────────────────────────────────────────────────────────

void AppCommandHandler::Disconnected(const std::string& connection)
{
    _app.Service().ReleaseControls(connection);
}

std::string AppCommandHandler::Execute(const std::string& json, const std::string& connection)
{
    std::string command = JsonGetString(json, "command");

    if (command.empty())
    {
        return "{\"ok\":false,\"error\":\"missing 'command' field\"}";
    }

    LAppDelegate* app = &_app;
    LAppLive2DManager* mgr = &_models;
    const auto stream = JsonGetString(json, "stream_source");
    const bool streaming = stream == "true" || stream == "1";
    const std::string source = streaming ? connection : "cli";
    const auto lifetime = streaming ? ExternalControlState::Lifetime::Stream
        : ExternalControlState::Lifetime::Persistent;
    if (streaming && connection.empty()) return "{\"ok\":false,\"error\":\"stream requires a connection\"}";

    // ── get_available_models ──
    if (command == "get_available_models")
    {
        csmVector<csmString> dirs = mgr->GetModelDir();
        std::ostringstream oss;
        oss << "{\"ok\":true,\"models\":[";
        for (csmInt32 i = 0; i < (csmInt32)dirs.GetSize(); i++)
        {
            if (i > 0) oss << ",";
            oss << "\"" << JsonEscape(dirs[i].GetRawString()) << "\"";
        }
        oss << "]}";
        return oss.str();
    }

    // ── get_current_model ──
    if (command == "get_current_model")
    {
        csmVector<csmString> dirs = mgr->GetModelDir();
        csmInt32 idx = mgr->GetSceneIndex();
        std::string name = "";
        if (idx >= 0 && idx < (csmInt32)dirs.GetSize())
        {
            name = dirs[idx].GetRawString();
        }
        std::ostringstream oss;
        oss << "{\"ok\":true,\"model\":\"" << JsonEscape(name) << "\",\"index\":" << idx << "}";
        return oss.str();
    }

    // ── set_model ──
    if (command == "set_model")
    {
        std::string name = JsonGetString(json, "name");
        int index = JsonGetInt(json, "index", -1);

        if (!name.empty())
        {
            csmVector<csmString> dirs = mgr->GetModelDir();
            for (csmInt32 i = 0; i < (csmInt32)dirs.GetSize(); i++)
            {
                if (strcmp(dirs[i].GetRawString(), name.c_str()) == 0)
                {
                    index = i;
                    break;
                }
            }
            if (index < 0)
            {
                return "{\"ok\":false,\"error\":\"model not found\"}";
            }
        }

        if (index < 0 || index >= mgr->GetModelDirSize())
        {
            return "{\"ok\":false,\"error\":\"invalid index\"}";
        }

        return Reply(app->Service().SelectModel(index));
    }

    if (command == "get_hotkeys" || command == "trigger_hotkey")
    {
        LAppModel* model = mgr->GetModel(0);
        if (!model) return "{\"ok\":false,\"error\":\"no model loaded\"}";
        auto& hotkeys = model->Hotkeys();
        if (command == "trigger_hotkey")
        {
            if (JsonGetString(json, "model_token") != hotkeys.Token())
                return "{\"ok\":false,\"error\":\"Model changed; refresh hotkeys before triggering\"}";
            const auto result = app->Service().TriggerHotkey(JsonGetString(json, "id"));
            if (!result.ok) return Reply(result);
        }
        std::ostringstream out;
        out << "{\"ok\":true,\"model_token\":\"" << hotkeys.Token()
            << "\",\"warning\":\"" << JsonEscape(hotkeys.Error()) << "\",\"hotkeys\":[";
        bool first = true;
        for (const auto& binding : hotkeys.Bindings())
        {
            if (!first) out << ',';
            first = false;
            out << "{\"id\":\"" << JsonEscape(binding.id) << "\",\"name\":\"" << JsonEscape(binding.name)
                << "\",\"action\":\"" << JsonEscape(binding.action) << "\",\"file\":\"" << JsonEscape(binding.file)
                << "\",\"reason\":\"" << JsonEscape(binding.reason) << "\",\"enabled\":" << (binding.enabled ? "true" : "false")
                << ",\"active\":" << (hotkeys.IsActive(binding) ? "true" : "false") << ",\"keys\":[";
            for (size_t i = 0; i < binding.keys.size(); ++i)
            {
                if (i) out << ',';
                out << '"' << JsonEscape(binding.keys[i]) << '"';
            }
            out << "]}";
        }
        out << "]}";
        return out.str();
    }

    // ── get_expressions ──
    if (command == "get_expressions")
    {
        LAppModel* model = mgr->GetModel(0);
        if (!model)
        {
            return "{\"ok\":false,\"error\":\"no model loaded\"}";
        }

        std::vector<std::string> exprs = model->GetExpressionIds();
        std::ostringstream oss;
        oss << "{\"ok\":true,\"expressions\":[";
        for (size_t i = 0; i < exprs.size(); i++)
        {
            if (i > 0) oss << ",";
            oss << "\"" << JsonEscape(exprs[i]) << "\"";
        }
        oss << "]}";
        return oss.str();
    }

    // ── set_expression ──
    if (command == "set_expression")
    {
        LAppModel* model = mgr->GetModel(0);
        if (!model)
        {
            return "{\"ok\":false,\"error\":\"no model loaded\"}";
        }

        std::string id = JsonGetString(json, "id");
        if (id.empty())
        {
            return "{\"ok\":false,\"error\":\"missing 'id'\"}";
        }

        return Reply(app->Service().SetExpression(id));
    }

    // ── get_motions ──
    if (command == "get_motions")
    {
        LAppModel* model = mgr->GetModel(0);
        if (!model)
        {
            return "{\"ok\":false,\"error\":\"no model loaded\"}";
        }

        std::vector<LAppModel::MotionInfo> motions = model->GetMotionList();
        std::ostringstream oss;
        oss << "{\"ok\":true,\"motions\":[";
        for (size_t i = 0; i < motions.size(); i++)
        {
            if (i > 0) oss << ",";
            oss << "{\"group\":\"" << JsonEscape(motions[i].group) << "\""
                << ",\"index\":" << motions[i].index
                << ",\"file\":\"" << JsonEscape(motions[i].file) << "\"}";
        }
        oss << "]}";
        return oss.str();
    }

    // ── do_motion ──
    if (command == "do_motion")
    {
        LAppModel* model = mgr->GetModel(0);
        if (!model)
        {
            return "{\"ok\":false,\"error\":\"no model loaded\"}";
        }

        std::string group = JsonGetString(json, "group");
        int index = JsonGetInt(json, "index", 0);
        int priority = JsonGetInt(json, "priority", LAppDefine::PriorityNormal);

        return Reply(app->Service().PlayMotion(group, index, priority));
    }

    // ── set_mouth_y ──
    if (command == "set_mouth_y")
    {
        std::string reset = JsonGetString(json, "reset");
        if (reset == "true" || reset == "1") {
            app->Service().ResetMouth(source);
            return "{\"ok\":true}";
        }
        float value = JsonGetFloat(json, "value", 0.0f);
        if (value < 0.0f) value = 0.0f;
        if (value > 1.0f) value = 1.0f;
        return Reply(app->Service().SetMouth(source, lifetime, value));
    }

    // ── set_model_zoom ──
    if (command == "set_model_zoom")
    {
        float zoom = JsonGetFloat(json, "value", -1.0f);
        if (zoom < 0.1f || zoom > 10.0f)
        {
            return "{\"ok\":false,\"error\":\"value must be 0.1..10.0\"}";
        }
        auto transform = app->Service().Snapshot().transform;
        transform.scale = zoom;
        return Reply(app->Service().SetTransform(transform));
    }

    // ── set_model_position ──
    if (command == "set_model_position")
    {
        auto transform = app->Service().Snapshot().transform;
        transform.x = JsonGetFloat(json, "x", transform.x);
        transform.y = JsonGetFloat(json, "y", transform.y);
        return Reply(app->Service().SetTransform(transform));
    }

    // ── get_model_position ──
    if (command == "get_model_position")
    {
        std::ostringstream oss;
        const auto transform = app->Service().Snapshot().transform;
        oss << "{\"ok\":true,\"x\":" << transform.x
            << ",\"y\":" << transform.y
            << ",\"zoom\":" << transform.scale << "}";
        return oss.str();
    }

    // ── toggle_hidden ──
    if (command == "toggle_hidden")
    {
        auto result = app->Service().SetVisibility(app->Service().Snapshot().hidden);
        if (!result.ok) return Reply(result);
        std::ostringstream oss;
        oss << "{\"ok\":true,\"hidden\":" << (app->Service().Snapshot().hidden ? "true" : "false") << "}";
        return oss.str();
    }

    // ── set_look ──
    if (command == "set_look")
    {
        std::string reset = JsonGetString(json, "reset");
        if (reset == "true" || reset == "1")
        {
            app->Service().ResetLook(source);
            return "{\"ok\":true}";
        }
        float x = JsonGetFloat(json, "x", 0.0f);
        float y = JsonGetFloat(json, "y", 0.0f);
        return Reply(app->Service().SetLook(source, lifetime, x, y));
    }

    if (command == "release_source")
    {
        app->Service().ReleaseControls(source);
        return "{\"ok\":true}";
    }

    if (command == "get_control_status")
    {
        const auto frame = app->Service().FrameInput();
        std::ostringstream result;
        result << "{\"ok\":true,\"has_mouth\":" << (frame.hasMouth ? "true" : "false")
            << ",\"mouth\":" << frame.mouth
            << ",\"has_look\":" << (frame.hasLook ? "true" : "false")
            << ",\"look_x\":" << frame.lookX << ",\"look_y\":" << frame.lookY << "}";
        return result.str();
    }

    // ── get_status ──
    if (command == "get_status")
    {
        csmVector<csmString> dirs = mgr->GetModelDir();
        const auto state = app->Service().Snapshot();
        csmInt32 idx = state.modelIndex;
        std::string modelName = "";
        if (idx >= 0 && idx < (csmInt32)dirs.GetSize())
        {
            modelName = dirs[idx].GetRawString();
        }

        std::ostringstream oss;
        oss << "{\"ok\":true"
            << ",\"model\":\"" << JsonEscape(modelName) << "\""
            << ",\"model_index\":" << idx
            << ",\"model_count\":" << state.modelCount
            << ",\"hidden\":" << (state.hidden ? "true" : "false")
            << ",\"zoom\":" << state.transform.scale
            << ",\"x\":" << state.transform.x
            << ",\"y\":" << state.transform.y
            << "}";
        return oss.str();
    }

    // ── next_model ──
    if (command == "next_model")
    {
        auto state = app->Service().Snapshot();
        return state.modelCount ? Reply(app->Service().SelectModel((state.modelIndex + 1) % state.modelCount))
            : "{\"ok\":false,\"error\":\"no models available\"}";
    }

    // ── prev_model ──
    if (command == "prev_model")
    {
        csmInt32 size = mgr->GetModelDirSize();
        if (size > 0)
        {
            csmInt32 idx = mgr->GetSceneIndex();
            csmInt32 prev = (idx - 1 + size) % size;
            return Reply(app->Service().SelectModel(prev));
        }
        return "{\"ok\":false,\"error\":\"no models available\"}";
    }

    // ── switch_skin ──
    if (command == "switch_skin")
    {
        return Reply(app->Service().SwitchSkin());
    }

    return "{\"ok\":false,\"error\":\"unknown command\"}";
}
