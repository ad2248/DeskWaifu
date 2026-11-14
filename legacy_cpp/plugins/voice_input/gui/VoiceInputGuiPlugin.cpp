#include "VoiceInputTab.hpp"
#include "plugin/GuiPluginApi.hpp"
#include "plugin/GuiPluginContext.hpp"
#include <memory>

// Independent GUI entry linked into voice_input.so alongside the backend entry.
class VoiceInputGuiPlugin final : public IGuiPlugin {
public:
    bool Initialize(GuiPluginContext& context, QString* error) override
    {
        auto page = std::make_unique<VoiceInputTab>(*context.Services());
        if (!context.RegisterTab({ QStringLiteral("voice_input"), QStringLiteral("语音输入"), 35 }, page.get(), error)) {
            return false;
        }
        _page = page.release();
        return true;
    }
    bool Start(QString*) override
    {
        _page->start();
        return true;
    }
    void Stop() noexcept override
    {
        if (_page) {
            _page->stop();
        }
    }

private:
    VoiceInputTab* _page = nullptr;
};
DESKWAIFU_EXPORT_GUI_PLUGIN(VoiceInputGuiPlugin)
