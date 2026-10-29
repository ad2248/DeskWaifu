#include "plugin/PluginApi.hpp"
#include "plugin/PluginContext.hpp"
#include "processor/interaction/TransformEditor.hpp"
#include "processor/interaction/ScreenCropInteraction.hpp"
#include "processor/interaction/config/HoverOpacitySettings.hpp"
#include "processor/interaction/config/MouseLookSettings.hpp"
#include "processor/interaction/config/RenderDeviceSettings.hpp"
#include <QJsonDocument>

namespace {
struct Runtime : IApplicationRuntime
{
    int ModelIndex() const override { return 0; }
    int ModelCount() const override { return 1; }
    bool SetHidden(bool) override { return true; }
};
// Test-only protocol adapter. The GUI integration test cannot include or link
// desktop implementation; these are the same settings objects its old fake
// backend used, now owned and compiled exclusively by desktop.
class DesktopSettingsFixture final : public IProcessorPlugin
{
public:
    bool Initialize(PluginContext& context, QString*) override
    {
        _endpoint = context.CreateEndpoint("settings", [this](const EventDelivery& delivery) {
            if (delivery.kind != EventDelivery::Kind::Request || !delivery.payload) return;
            const auto input = delivery.payload->data.value("input").toJsonObject();
            const auto command = input.value("command").toString();
            const auto owner = delivery.payload->data.value("connection").toString().toStdString();
            AppResult result = AppResult::Success();
            if (command == "test.initialize") {
                _config.sourcePath = input.value("path").toString().toStdString();
                _editor.reset(new TransformEditor(_service, _config));
                _hover.reset(new HoverOpacitySettings(_config));
                _look.reset(new MouseLookSettings(_config));
                _render.reset(new RenderDeviceSettings(_config));
                if (!_editor->Initialize({})) result = AppResult::Failure("initialize editor");
            } else if (command == "get_hover_opacity") result = _hover->Refresh();
            else if (command == "set_hover_opacity") result = _hover->Set(QJsonDocument(input).toJson().toStdString());
            else if (command == "get_mouse_look") result = _look->Refresh();
            else if (command == "set_mouse_look") result = _look->Set(QJsonDocument(input).toJson().toStdString());
            else if (command == "get_render_device") result = _render->Refresh();
            else if (command == "set_render_device") {
                ConfigStore::Snapshot parsed; std::string error;
                result = ConfigStore::Parse(QJsonDocument(input).toJson().toStdString(), parsed, error)
                    ? _render->Set(parsed) : AppResult::Failure(error);
            } else if (command == "begin_transform_edit") result = _editor->Begin(owner, input.value("screen").toBool(), 1920, 1080);
            else if (command == "update_transform_edit") {
                auto draft = _editor->Display();
                if (input.contains("x")) draft.transform.x = float(input.value("x").toDouble());
                if (input.contains("y")) draft.transform.y = float(input.value("y").toDouble());
                if (input.contains("zoom")) draft.transform.scale = float(input.value("zoom").toDouble());
                if (input.contains("cropped")) draft.cropped = input.value("cropped").toBool();
                if (input.contains("crop_x")) draft.region.x = input.value("crop_x").toInt();
                if (input.contains("crop_y")) draft.region.y = input.value("crop_y").toInt();
                if (input.contains("crop_width")) draft.region.width = input.value("crop_width").toInt();
                if (input.contains("crop_height")) draft.region.height = input.value("crop_height").toInt();
                result = _editor->Update(owner, draft);
            } else if (command == "apply_transform_edit") result = _editor->Apply(owner);
            else if (command == "cancel_transform_edit") result = _editor->Cancel(owner);
            else if (command == "reset_transform_settings") result = _editor->Reset(owner);
            else if (command == "test.reload") _editor->Reload();
            else if (command == "test.disconnect") _editor->Disconnected(owner);
            else if (command == "test.drag") {
                if (!_pointer.StartModelDrag(*_editor, 100, 100, 120, 100, 1920, 1080)) result = AppResult::Failure("start model drag");
                else result = _pointer.Release(*_editor, 100, 120, 100);
            } else if (command == "test.press_release") {
                _pointer.Press(_editor->Display(), 0, 200);
                result = _pointer.Release(*_editor, uint32_t(input.value("time").toInt()), 0, 200);
            }
            auto response = Snapshot(owner);
            response.insert("ok", result.ok);
            response.insert("error", QString::fromStdString(result.error));
            if (command == "get_transform_save_result") response.insert("known", true);
            _endpoint->Respond(delivery.requestId, std::make_shared<ProcessorEvent>("test.settings.result", nullptr, nullptr,
                QVariantMap{{"response", response}}));
        });
        return _endpoint && _endpoint->Provide("test.desktop.settings/v1");
    }
    void Stop() noexcept override
    {
        _editor.reset(); _hover.reset(); _look.reset(); _render.reset(); _endpoint = nullptr;
    }
private:
    QJsonObject Snapshot(const std::string& owner) const
    {
        if (!_editor) return {};
        const auto display = _editor->Display();
        const auto runtime = _service.Snapshot().transform;
        return {{"x", display.transform.x}, {"y", display.transform.y}, {"zoom", display.transform.scale},
            {"cropped", display.cropped}, {"crop_x", display.region.x}, {"crop_y", display.region.y},
            {"crop_width", display.region.width}, {"crop_height", display.region.height},
            {"surface_width", 1920}, {"surface_height", 1080}, {"owned", _editor->OwnedBy(owner)},
            {"editing", _editor->Editing()}, {"local_screen_editing", _editor->LocalScreenEditing()},
            {"revision", int(_editor->Revision())}, {"config_path", QString::fromStdString(_config.sourcePath)},
            {"enabled", _hover->Enabled()}, {"opacity", _hover->Opacity()}, {"idle_opacity", _hover->IdleOpacity()},
            {"radius", _look->Radius()}, {"device", QString::fromStdString(_render->Device())},
            {"runtime_x", runtime.x}, {"runtime_zoom", runtime.scale}};
    }
    EventEndpoint* _endpoint = nullptr;
    Runtime _runtime;
    ApplicationService _service{_runtime};
    LAppConfig _config;
    std::unique_ptr<TransformEditor> _editor;
    std::unique_ptr<HoverOpacitySettings> _hover;
    std::unique_ptr<MouseLookSettings> _look;
    std::unique_ptr<RenderDeviceSettings> _render;
    ScreenCropInteraction _pointer;
};
}
DESKWAIFU_EXPORT_PLUGIN(DesktopSettingsFixture)
