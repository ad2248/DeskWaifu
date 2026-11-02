#include "LogTab.hpp"
#include "plugin/GuiPluginApi.hpp"
#include "plugin/GuiPluginContext.hpp"
#include <memory>

class LogGuiPlugin final : public IGuiPlugin
{
public:
    bool Initialize(GuiPluginContext& context, QString* error) override
    {
        auto page = std::make_unique<LogTab>(*context.Services());
        if (!context.RegisterTab({QStringLiteral("log"), QStringLiteral("日志"), 100}, page.get(), error)) return false;
        page.release();
        return true;
    }
    void Stop() noexcept override {}
};
DESKWAIFU_EXPORT_GUI_PLUGIN(LogGuiPlugin)
