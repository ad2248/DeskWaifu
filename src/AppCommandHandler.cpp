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
    return (float)atof(val.c_str());
}

static int JsonGetInt(const std::string& json, const std::string& key, int defaultVal)
{
    std::string val = JsonGetString(json, key);
    if (val.empty()) return defaultVal;
    return atoi(val.c_str());
}

// ─── Command handlers ─────────────────────────────────────────────────────────

std::string AppCommandHandler::Execute(const std::string& json)
{
    std::string command = JsonGetString(json, "command");

    if (command.empty())
    {
        return "{\"ok\":false,\"error\":\"missing 'command' field\"}";
    }

    LAppDelegate* app = &_app;
    LAppLive2DManager* mgr = &_models;

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

        return mgr->ChangeScene(index) ? "{\"ok\":true}"
            : "{\"ok\":false,\"error\":\"model load failed\"}";
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
            std::string error;
            if (!hotkeys.Trigger(JsonGetString(json, "id"), error))
                return "{\"ok\":false,\"error\":\"" + JsonEscape(error) + "\"}";
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

        model->SetExpression(id.c_str());
        return "{\"ok\":true}";
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

        if (group.empty())
        {
            // Try random auto-motion
            model->StartRandomMotion(LAppDefine::MotionGroupTapBody, priority);
            return "{\"ok\":true}";
        }

        model->StartMotion(group.c_str(), index, priority);
        return "{\"ok\":true}";
    }

    // ── set_mouth_y ──
    if (command == "set_mouth_y")
    {
        float value = JsonGetFloat(json, "value", 0.0f);
        if (value < 0.0f) value = 0.0f;
        if (value > 1.0f) value = 1.0f;
        _control.SetMouth(value);
        return "{\"ok\":true}";
    }

    // ── set_model_zoom ──
    if (command == "set_model_zoom")
    {
        float zoom = JsonGetFloat(json, "value", -1.0f);
        if (zoom < 0.1f || zoom > 10.0f)
        {
            return "{\"ok\":false,\"error\":\"value must be 0.1..10.0\"}";
        }
        app->_modelScale = zoom;
        return "{\"ok\":true}";
    }

    // ── set_model_position ──
    if (command == "set_model_position")
    {
        float x = JsonGetFloat(json, "x", app->_modelX);
        float y = JsonGetFloat(json, "y", app->_modelY);
        app->_modelX = x;
        app->_modelY = y;
        return "{\"ok\":true}";
    }

    // ── get_model_position ──
    if (command == "get_model_position")
    {
        std::ostringstream oss;
        oss << "{\"ok\":true,\"x\":" << app->_modelX
            << ",\"y\":" << app->_modelY
            << ",\"zoom\":" << app->_modelScale << "}";
        return oss.str();
    }

    // ── toggle_hidden ──
    if (command == "toggle_hidden")
    {
        app->ToggleHidden();
        std::ostringstream oss;
        oss << "{\"ok\":true,\"hidden\":" << (app->_isHidden ? "true" : "false") << "}";
        return oss.str();
    }

    // ── set_look ──
    if (command == "set_look")
    {
        std::string reset = JsonGetString(json, "reset");
        if (reset == "true" || reset == "1")
        {
            _control.ResetLook();
            return "{\"ok\":true}";
        }
        float x = JsonGetFloat(json, "x", 0.0f);
        float y = JsonGetFloat(json, "y", 0.0f);
        _control.SetLook(x, y);
        return "{\"ok\":true}";
    }

    // ── get_status ──
    if (command == "get_status")
    {
        csmVector<csmString> dirs = mgr->GetModelDir();
        csmInt32 idx = mgr->GetSceneIndex();
        std::string modelName = "";
        if (idx >= 0 && idx < (csmInt32)dirs.GetSize())
        {
            modelName = dirs[idx].GetRawString();
        }

        std::ostringstream oss;
        oss << "{\"ok\":true"
            << ",\"model\":\"" << JsonEscape(modelName) << "\""
            << ",\"model_index\":" << idx
            << ",\"model_count\":" << mgr->GetModelDirSize()
            << ",\"hidden\":" << (app->_isHidden ? "true" : "false")
            << ",\"zoom\":" << app->_modelScale
            << ",\"x\":" << app->_modelX
            << ",\"y\":" << app->_modelY
            << "}";
        return oss.str();
    }

    // ── next_model ──
    if (command == "next_model")
    {
        return mgr->NextScene() ? "{\"ok\":true}"
            : "{\"ok\":false,\"error\":\"model load failed\"}";
    }

    // ── prev_model ──
    if (command == "prev_model")
    {
        csmInt32 size = mgr->GetModelDirSize();
        if (size > 0)
        {
            csmInt32 idx = mgr->GetSceneIndex();
            csmInt32 prev = (idx - 1 + size) % size;
            return mgr->ChangeScene(prev) ? "{\"ok\":true}"
                : "{\"ok\":false,\"error\":\"model load failed\"}";
        }
        return "{\"ok\":false,\"error\":\"no models available\"}";
    }

    // ── switch_skin ──
    if (command == "switch_skin")
    {
        mgr->SwitchSkin();
        return "{\"ok\":true}";
    }

    return "{\"ok\":false,\"error\":\"unknown command\"}";
}
