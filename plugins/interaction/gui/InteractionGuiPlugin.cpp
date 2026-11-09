#include "InteractionTab.hpp"
#include "plugin/GuiPluginApi.hpp"
#include "plugin/GuiPluginContext.hpp"
#include <memory>

class InteractionGuiPlugin final : public IGuiPlugin
{
public:
    bool Initialize(GuiPluginContext& context, QString* error) override
    {
        auto page = std::make_unique<InteractionTab>(*context.Services());
        if (!context.RegisterTab({QStringLiteral("interaction"), QStringLiteral("交互"), 10}, page.get(), error)) return false;
        _page = page.release();
        return true;
    }
    bool Start(QString*) override { _page->start(); return true; }
    void Stop() noexcept override {}
private:
    InteractionTab* _page = nullptr;
};
DESKWAIFU_EXPORT_GUI_PLUGIN(InteractionGuiPlugin)
