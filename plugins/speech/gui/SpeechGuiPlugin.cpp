#include "SpeechTab.hpp"
#include "plugin/GuiPluginApi.hpp"
#include "plugin/GuiPluginContext.hpp"
#include <memory>

// This entry is linked into speech.so alongside its independent backend entry.
class SpeechGuiPlugin final : public IGuiPlugin
{
public:
    bool Initialize(GuiPluginContext& context, QString* error) override
    {
        auto page = std::make_unique<SpeechTab>(*context.Services());
        if (!context.RegisterTab({QStringLiteral("speech"), QStringLiteral("语音"), 30}, page.get(), error)) return false;
        _page = page.release();
        return true;
    }
    bool Start(QString*) override { _page->start(); return true; }
    void Stop() noexcept override {}
private:
    SpeechTab* _page = nullptr;
};
DESKWAIFU_EXPORT_GUI_PLUGIN(SpeechGuiPlugin)
