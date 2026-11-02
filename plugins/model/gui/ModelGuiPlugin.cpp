#include "ModelTab.hpp"
#include "plugin/GuiPluginApi.hpp"
#include "plugin/GuiPluginContext.hpp"
#include <memory>

class ModelGuiPlugin final : public IGuiPlugin
{
public:
    bool Initialize(GuiPluginContext& context, QString* error) override
    {
        auto page = std::make_unique<ModelTab>(*context.Services());
        if (!context.RegisterTab({QStringLiteral("model"), QStringLiteral("模型"), 20}, page.get(), error)) return false;
        _page = page.release();
        return true;
    }
    bool Start(QString*) override { _page->start(); return true; }
    void Stop() noexcept override {}
private:
    ModelTab* _page = nullptr;
};
DESKWAIFU_EXPORT_GUI_PLUGIN(ModelGuiPlugin)
