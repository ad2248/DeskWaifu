#include "SystemTab.hpp"
#include "plugin/GuiPluginApi.hpp"
#include "plugin/GuiPluginContext.hpp"
#include <memory>

class SystemGuiPlugin final : public IGuiPlugin
{
public:
    bool Initialize(GuiPluginContext& context, QString* error) override
    {
        auto page = std::make_unique<SystemTab>(*context.Services());
        if (!context.RegisterTab({QStringLiteral("system"), QStringLiteral("系统"), 0}, page.get(), error)) return false;
        _page = page.release();
        return true;
    }
    bool Start(QString*) override { _page->start(); return true; }
    void Stop() noexcept override { if (_page) _page->stop(); }
private:
    SystemTab* _page = nullptr; // Context-owned, valid through Stop.
};
DESKWAIFU_EXPORT_GUI_PLUGIN(SystemGuiPlugin)
