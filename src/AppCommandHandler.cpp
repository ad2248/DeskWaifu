#include "AppCommandHandler.hpp"
#include "JsonHelpers.hpp"
#include "LAppDelegate.hpp"
#include "processor/model/ModelProcessor.hpp"
#include "LAppDefine.hpp"
#include "MouseLookSettings.hpp"
#include "SpeechSettings.hpp"
#include "RenderDeviceSettings.hpp"
#include "OffscreenRenderer.hpp"
#include "OcrClient.hpp"
#include "processor/ProcessorRuntime.hpp"
#include <QJsonDocument>
#include <cstdlib>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <limits>
#include <stdexcept>

// ─── JSON 回复 ───────────────────────────────────────────────────────────────

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
        default:
            if (static_cast<unsigned char>(c) < 32) {
                const char* hex = "0123456789abcdef";
                out += "\\u00"; out += hex[(static_cast<unsigned char>(c) >> 4) & 15]; out += hex[c & 15];
            } else out += c;
            break;
        }
    }
    return out;
}

static std::string Reply(const AppResult& result)
{
    return result.ok ? "{\"ok\":true}"
        : "{\"ok\":false,\"error\":\"" + JsonEscape(result.error) + "\"}";
}

// ─── 命令处理 ─────────────────────────────────────────────────────────────────

void AppCommandHandler::Disconnected(const std::string& connection)
{
    _app.Settings().Disconnected(connection);
    _app.Service().ReleaseControls(connection);
}

std::string AppCommandHandler::Execute(const std::string& json, const std::string& connection)
try
{
    ConfigStore::Snapshot request;
    std::string error;
    if (!ConfigStore::Parse(json, request, error, false))
        return Reply(AppResult::Failure("Invalid JSON request or duplicate key"));
    std::string command = JsonGetString(request, "command");

    if (command.empty())
    {
        return "{\"ok\":false,\"error\":\"missing 'command' field\"}";
    }

    if (command == "reading_clip") {
        return _app.Processors().ReadClipboard() ? "{\"ok\":true,\"queued\":true}" : "{\"ok\":false,\"error\":\"event was not queued\"}";
    }
    if (command == "get_reading_clip_status")
        return QJsonDocument(QJsonObject::fromVariantMap(_app.Processors().Reading().Status())).toJson(QJsonDocument::Compact).toStdString();

    if (command == "get_ocr_status" || command == "ocr_recognize" ||
        command == "get_ocr_result" || command == "cancel_ocr") {
        auto& ocr = _app.Ocr();
        QJsonObject reply;
        if (command == "get_ocr_status") reply = ocr.Status();
        else if (command == "ocr_recognize") reply = ocr.Submit(QString::fromStdString(JsonGetString(request, "image")));
        else if (command == "get_ocr_result") reply = ocr.Result(QString::fromStdString(JsonGetString(request, "job_id")));
        else reply = ocr.Cancel(QString::fromStdString(JsonGetString(request, "job_id")));
        return QJsonDocument(reply).toJson(QJsonDocument::Compact).toStdString();
    }

    LAppDelegate* app = &_app;
    const auto& metadata = app->Models().Metadata();
    const bool streaming = JsonGetBool(request, "stream_source");
    const std::string source = streaming ? connection : "cli";
    const auto lifetime = streaming ? ExternalControlState::Lifetime::Stream
        : ExternalControlState::Lifetime::Persistent;
    if (streaming && connection.empty()) return "{\"ok\":false,\"error\":\"stream requires a connection\"}";

    if (command == "get_render_device" || command == "set_render_device") {
        RenderDeviceSettings settings(LAppConfig::GetInstance());
        const auto devices = ListRenderDevices();
        AppResult result = AppResult::Success();
        if (command == "set_render_device") {
            std::string requested;
            auto value = request.values.find("device");
            if (value == request.values.end() || !ConfigStore::ParseString(value->second, requested))
                result = AppResult::Failure("Specify device as a string");
            else if (!requested.empty() && requested != "auto" && std::none_of(devices.begin(), devices.end(),
                [&](const RenderDeviceInfo& device) { return device.pciAddress == requested; }))
                result = AppResult::Failure("Requested render device is unavailable");
            else result = settings.Set(request);
        } else result = settings.Refresh();
        const char* startup = std::getenv("DESKWAIFU_RENDER_DEVICE");
        const std::string active = startup && *startup ? startup : "auto";
        std::string reply = std::string("{\"ok\":") + (result.ok ? "true" : "false") +
            ",\"device\":\"" + JsonEscape(settings.Device()) + "\",\"active_device\":\"" + JsonEscape(active) +
            "\",\"restart_required\":" + (settings.Device() != active ? "true" : "false") +
            ",\"error\":\"" + JsonEscape(result.error) + "\",\"devices\":[";
        for (size_t i = 0; i < devices.size(); ++i) {
            if (i) reply += ',';
            reply += "{\"id\":\"" + JsonEscape(devices[i].pciAddress) + "\",\"name\":\"" + JsonEscape(devices[i].name) + "\"}";
        }
        return reply + "]}";
    }

    if (command == "get_speech_settings" || command == "set_speech_settings") {
        SpeechSettings settings(LAppConfig::GetInstance());
        const auto result = command == "get_speech_settings" ? settings.Refresh() : settings.Set(request);
        return std::string("{\"ok\":") + (result.ok ? "true" : "false") +
            ",\"configured\":" + (settings.Configured() ? "true" : "false") +
            ",\"error\":\"" + JsonEscape(result.error) + "\"}";
    }

    if (command == "get_mouse_look" || command == "set_mouse_look") {
        MouseLookSettings settings(LAppConfig::GetInstance());
        const auto result = command == "get_mouse_look" ? settings.Refresh() : settings.Set(request);
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::setprecision(std::numeric_limits<float>::max_digits10)
            << "{\"ok\":" << (result.ok ? "true" : "false")
            << ",\"radius\":" << settings.Radius()
            << ",\"error\":\"" << JsonEscape(result.error) << "\"}";
        return out.str();
    }

    if (command == "get_hover_opacity" || command == "set_hover_opacity") {
        auto& settings = app->HoverSettings();
        const auto result = command == "get_hover_opacity" ? settings.Refresh() : settings.Set(request);
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::setprecision(std::numeric_limits<float>::max_digits10)
            << "{\"ok\":" << (result.ok ? "true" : "false")
            << ",\"enabled\":" << (settings.Enabled() ? "true" : "false")
            << ",\"opacity\":" << settings.Opacity()
            << ",\"idle_opacity\":" << settings.IdleOpacity()
            << ",\"error\":\"" << JsonEscape(result.error) << "\"}";
        return out.str();
    }

    auto& editor = app->Settings();
    if (command == "set_output") return Reply(app->SelectOutput(JsonGetString(request, "name")));
    if (editor.Editing() && (command == "set_model" || command == "next_model" || command == "prev_model" ||
        command == "switch_skin" || command == "trigger_hotkey" || command == "set_expression" || command == "do_motion"))
        return Reply(AppResult::Failure("Finish transform editing before changing model actions"));
    const auto requestId = JsonGetString(request, "request_id");
    if (requestId.size() > 128) return Reply(AppResult::Failure("Request identifier too long"));
    if (command == "get_transform_save_result") {
        auto it = _saveReceipts.find(requestId);
        return it == _saveReceipts.end() ? "{\"ok\":true,\"known\":false}" : it->second;
    }
    const bool saves = command == "apply_transform_edit" || command == "reset_transform_settings" || command == "retry_transform_save";
    if (saves && !requestId.empty()) {
        auto it = _saveReceipts.find(requestId);
        if (it != _saveReceipts.end()) return it->second;
    }
    if (command == "get_transform_settings" || command == "begin_transform_edit" ||
        command == "update_transform_edit" || command == "apply_transform_edit" ||
        command == "cancel_transform_edit" || command == "reset_transform_settings" ||
        command == "relocate_crop" || command == "retry_transform_save" || command == "reload_transform_settings")
    {
        editor.SyncRuntime();
        for (const auto* flag : {"screen", "overwrite", "cropped"}) {
            auto it = request.values.find(flag);
            if (it != request.values.end() && it->second != "true" && it->second != "false")
                return Reply(AppResult::Failure(std::string(flag) + " must be boolean"));
        }
        auto flag = [&](const char* key) { auto it = request.values.find(key); return it != request.values.end() && it->second == "true"; };
        AppResult result = AppResult::Success();
        if (command == "begin_transform_edit") {
            unsigned revision = std::numeric_limits<unsigned>::max();
            auto it = request.values.find("expected_revision");
            if (it != request.values.end()) {
                try {
                    if (it->second.empty() || it->second.find_first_not_of("0123456789") != std::string::npos)
                        throw std::invalid_argument("revision");
                    size_t end; auto n = std::stoull(it->second, &end);
                    if (end != it->second.size() || n >= std::numeric_limits<unsigned>::max()) throw std::out_of_range("revision");
                    revision = unsigned(n);
                }
                catch (...) { return Reply(AppResult::Failure("Invalid expected revision")); }
            }
            result = editor.Begin(connection, flag("screen"), app->GetWindowWidth(), app->GetWindowHeight(), revision);
        }
        else if (command == "update_transform_edit") {
            const auto previous = editor.Display();
            auto next = previous;
            next.transform.x = JsonGetFloat(request, "x", next.transform.x);
            next.transform.y = JsonGetFloat(request, "y", next.transform.y);
            next.transform.scale = JsonGetFloat(request, "zoom", next.transform.scale);
            next.region.x = JsonGetInt(request, "crop_x", next.region.x);
            next.region.y = JsonGetInt(request, "crop_y", next.region.y);
            next.region.width = JsonGetInt(request, "crop_width", next.region.width);
            next.region.height = JsonGetInt(request, "crop_height", next.region.height);
            auto cropped = request.values.find("cropped");
            if (cropped != request.values.end()) {
                if (cropped->second != "true" && cropped->second != "false") return Reply(AppResult::Failure("cropped must be boolean"));
                next.cropped = cropped->second == "true";
            }
            // 编辑 X/Y 数值时，与屏幕上的移动手柄一样同时移动模型和裁剪框。
            // 屏幕上的缩放操作不经过此适配器。
            if (next.cropped && previous.cropped && app->GetWindowHeight() > 0 && next.transform.scale > 0) {
                double units = 2.0 / app->GetWindowHeight() / next.transform.scale;
                next.transform.x += (int64_t(next.region.x) - previous.region.x) * units;
                next.transform.y -= (int64_t(next.region.y) - previous.region.y) * units;
            }
            result = editor.Update(connection, next);
        }
        else if (command == "apply_transform_edit") result = editor.Apply(connection, flag("overwrite"));
        else if (command == "cancel_transform_edit") result = editor.Cancel(connection);
        else if (command == "reset_transform_settings") result = editor.Reset(connection, flag("overwrite"));
        else if (command == "reload_transform_settings") result = editor.ReloadAndDiscard(connection);
        else if (command == "retry_transform_save") {
            if (editor.Editing()) result = AppResult::Failure("Finish editing before retrying save");
            else result = editor.Retry();
        }
        else if (command == "relocate_crop") {
            auto next = editor.Display();
            auto r = next.region;
            // 模型与裁剪框始终一起移动，恢复屏幕外的矩形时也不例外。
            int64_t dx = -int64_t(r.x), dy = -int64_t(r.y);
            result = editor.Update(connection, TransformEditor::MoveCrop(next, dx, dy, app->GetWindowHeight()));
        }
        if (!result.ok) {
            const auto reply = Reply(result);
            if (saves && !requestId.empty()) RememberSave(requestId, reply);
            return reply;
        }
        const auto& settings = editor.Display(); const auto r = settings.region;
        const auto effective = ResolveModelRegion(settings.cropped, r, app->GetWindowWidth(), app->GetWindowHeight());
        std::ostringstream out;
        out.imbue(std::locale::classic()); out << std::setprecision(std::numeric_limits<float>::max_digits10);
        out << "{\"ok\":true,\"x\":" << settings.transform.x << ",\"y\":" << settings.transform.y
            << ",\"zoom\":" << settings.transform.scale << ",\"cropped\":" << (settings.cropped ? "true" : "false")
            << ",\"crop_x\":" << r.x << ",\"crop_y\":" << r.y << ",\"crop_width\":" << r.width << ",\"crop_height\":" << r.height
            << ",\"effective_x\":" << effective.x << ",\"effective_y\":" << effective.y << ",\"effective_width\":" << effective.width << ",\"effective_height\":" << effective.height
            << ",\"surface_width\":" << app->GetWindowWidth() << ",\"surface_height\":" << app->GetWindowHeight()
            << ",\"revision\":" << editor.Revision() << ",\"editing\":" << (editor.Editing() ? "true" : "false")
            << ",\"owned\":" << (editor.OwnedBy(connection) ? "true" : "false")
            << ",\"screen_editing\":" << (editor.ScreenEditing() ? "true" : "false")
            << ",\"local_screen_editing\":" << (editor.LocalScreenEditing() ? "true" : "false")
            << ",\"unsaved\":" << (editor.Dirty() ? "true" : "false") << ",\"conflict\":" << (editor.Conflict() ? "true" : "false")
            << ",\"known\":true,\"config_path\":\"" << JsonEscape(editor.Path()) << "\",\"warning\":\"" << JsonEscape(editor.Error()) << "\""
            << ",\"outputs\":[";
        bool first = true;
        for (const auto& output : app->Outputs()) {
            if (!first) out << ',';
            first = false;
            out << "{\"name\":\"" << JsonEscape(output.name) << "\",\"current\":"
                << (output.id == app->CurrentOutput() ? "true" : "false") << '}';
        }
        out << "]}";
        const auto reply = out.str();
        if (saves && !requestId.empty()) {
            RememberSave(requestId, reply);
        }
        return reply;
    }

    // ── 获取可用模型 ──
    if (command == "get_available_models")
    {
        const auto& dirs = metadata.names;
        std::ostringstream oss;
        oss << "{\"ok\":true,\"models\":[";
        for (int i = 0; i < dirs.size(); i++)
        {
            if (i > 0) oss << ",";
            oss << "\"" << JsonEscape(dirs[i].toStdString()) << "\"";
        }
        oss << "]}";
        return oss.str();
    }

    // ── 获取当前模型 ──
    if (command == "get_current_model")
    {
        const auto& dirs = metadata.names;
        int idx = metadata.index;
        std::string name = "";
        if (idx >= 0 && idx < dirs.size())
        {
            name = dirs[idx].toStdString();
        }
        std::ostringstream oss;
        oss << "{\"ok\":true,\"model\":\"" << JsonEscape(name) << "\",\"index\":" << idx << "}";
        return oss.str();
    }

    if (command == "get_hotkeys")
        return QJsonDocument(metadata.hotkeys).toJson(QJsonDocument::Compact).toStdString();
    if (command == "get_expressions")
        return QJsonDocument(QJsonObject{{"ok", true}, {"expressions", metadata.expressions}}).toJson(QJsonDocument::Compact).toStdString();
    if (command == "get_motions")
        return QJsonDocument(QJsonObject{{"ok", true}, {"motions", metadata.motions}}).toJson(QJsonDocument::Compact).toStdString();

    // ── 设置嘴部开合 ──
    if (command == "set_mouth_y")
    {
        if (JsonGetBool(request, "reset")) {
            app->Service().ResetMouth(source);
            return "{\"ok\":true}";
        }
        float value = JsonGetFloat(request, "value", 0.0f);
        if (value < 0.0f) value = 0.0f;
        if (value > 1.0f) value = 1.0f;
        return Reply(app->Service().SetMouth(source, lifetime, value));
    }

    // ── 设置模型缩放 ──
    if (command == "set_model_zoom")
    {
        if (editor.Editing()) return Reply(AppResult::Failure("Transform editor is active"));
        float zoom = JsonGetFloat(request, "value", -1.0f);
        if (zoom < 0.1f || zoom > 10.0f)
        {
            return "{\"ok\":false,\"error\":\"value must be 0.1..10.0\"}";
        }
        auto transform = app->Service().Snapshot().transform;
        transform.scale = zoom;
        auto result = app->Service().SetTransform(transform);
        if (result.ok && !streaming) { editor.ScheduleSave(0, 0); result = editor.Retry(); }
        return Reply(result);
    }

    // ── 设置模型位置 ──
    if (command == "set_model_position")
    {
        if (editor.Editing()) return Reply(AppResult::Failure("Transform editor is active"));
        auto transform = app->Service().Snapshot().transform;
        transform.x = JsonGetFloat(request, "x", transform.x);
        transform.y = JsonGetFloat(request, "y", transform.y);
        auto result = app->Service().SetTransform(transform);
        if (result.ok && !streaming) { editor.ScheduleSave(0, 0); result = editor.Retry(); }
        return Reply(result);
    }

    // ── 获取模型位置 ──
    if (command == "get_model_position")
    {
        std::ostringstream oss;
        const auto transform = app->Service().Snapshot().transform;
        oss << "{\"ok\":true,\"x\":" << transform.x
            << ",\"y\":" << transform.y
            << ",\"zoom\":" << transform.scale << "}";
        return oss.str();
    }

    // ── 切换隐藏状态 ──
    if (command == "toggle_hidden")
    {
        auto result = app->Service().SetVisibility(app->Service().Snapshot().hidden);
        if (!result.ok) return Reply(result);
        std::ostringstream oss;
        oss << "{\"ok\":true,\"hidden\":" << (app->Service().Snapshot().hidden ? "true" : "false") << "}";
        return oss.str();
    }

    // ── 设置视线 ──
    if (command == "set_look")
    {
        if (JsonGetBool(request, "reset"))
        {
            app->Service().ResetLook(source);
            return "{\"ok\":true}";
        }
        float x = JsonGetFloat(request, "x", 0.0f);
        float y = JsonGetFloat(request, "y", 0.0f);
        return Reply(app->Service().SetLook(source, lifetime, x, y));
    }

    if (command == "release_source")
    {
        app->Service().ReleaseControls(source);
        return "{\"ok\":true}";
    }

    if (command == "quit")
    {
        app->AppEnd();
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

    // ── 获取状态 ──
    if (command == "get_status")
    {
        const auto& dirs = metadata.names;
        const auto state = app->Service().Snapshot();
        int idx = state.modelIndex;
        std::string modelName = "";
        if (idx >= 0 && idx < dirs.size())
        {
            modelName = dirs[idx].toStdString();
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

    return "{\"ok\":false,\"error\":\"unknown command\"}";
}
catch (const std::invalid_argument& error)
{
    return Reply(AppResult::Failure(error.what()));
}

void AppCommandHandler::ExecuteAsync(const std::string& json, const std::string& connection,
    std::function<void(std::string)> complete)
{
    ConfigStore::Snapshot request; std::string error;
    if (!ConfigStore::Parse(json, request, error, false)) { complete(Execute(json, connection)); return; }
    std::string command;
    try { command = JsonGetString(request, "command"); }
    catch (const std::invalid_argument&) { complete(Execute(json, connection)); return; }
    if (command == "set_model" || command == "next_model" || command == "prev_model" ||
        command == "get_hotkeys" || command == "trigger_hotkey" || command == "set_expression" || command == "do_motion" || command == "switch_skin") {
        try {
            if (JsonGetBool(request, "stream_source") && connection.empty()) {
                complete("{\"ok\":false,\"error\":\"stream requires a connection\"}"); return;
            }
        } catch (const std::invalid_argument& error) { complete(Reply(AppResult::Failure(error.what()))); return; }
        if (!_app.Models().Submit(json, [complete](QJsonObject reply) {
            complete(QJsonDocument(reply).toJson(QJsonDocument::Compact).toStdString());
        })) complete("{\"ok\":false,\"error\":\"model renderer stopped\"}");
        return;
    }
    complete(Execute(json, connection));
}
