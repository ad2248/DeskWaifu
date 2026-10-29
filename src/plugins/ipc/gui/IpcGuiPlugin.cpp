#include "DeskWaifuClient.hpp"
#include "plugin/GuiPluginApi.hpp"
#include "plugin/GuiPluginContext.hpp"
#include <QPointer>
#include <memory>

class IpcGuiPlugin final : public IGuiPlugin
{
public:
    bool Initialize(GuiPluginContext& context, QString* error) override
    {
        if (error) error->clear();
        _services = context.Services();
        _client = std::make_unique<DeskWaifuClient>();
        if (_services->registerTransport(_client.get(), &context)) return true;
        if (error) *error = QStringLiteral("Cannot register GUI command transport");
        return false;
    }
    bool Start(QString* error) override
    {
        Q_UNUSED(error);
        _client->connectToServer();
        return true;
    }
    void Stop() noexcept override
    {
        if (_services && _client) _services->unregisterTransport(_client.get());
        _client.reset();
        _services.clear();
    }
private:
    QPointer<GuiHostServices> _services;
    std::unique_ptr<DeskWaifuClient> _client;
};

DESKWAIFU_EXPORT_GUI_PLUGIN(IpcGuiPlugin)
