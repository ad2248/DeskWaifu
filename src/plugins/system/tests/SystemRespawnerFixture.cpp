#include "plugin/GuiPluginApi.hpp"
#include "plugin/GuiPluginContext.hpp"
#include "DeskWaifuRespawner.hpp"
#include <QWidget>

class RespawnerPage final : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(bool running READ running)
public:
    explicit RespawnerPage(GuiHostServices& services) : _services(services) { setObjectName("respawnerFixture"); }
    bool running() const { return _respawner && _respawner->isRunning(); }
    Q_INVOKABLE void configure(const QString& directory)
    {
        _respawner.reset(new DeskWaifuRespawner(_services, directory));
        connect(_respawner.get(), &DeskWaifuRespawner::finished, this, &RespawnerPage::finished);
    }
    Q_INVOKABLE void start(const QString& proc, const QString& locks, int timeout)
    { _respawner->start(proc, locks, timeout); }
    Q_INVOKABLE void stop() { if (_respawner) _respawner->stop(); }
signals:
    void finished(bool success, const QString& message);
private:
    GuiHostServices& _services;
    std::unique_ptr<DeskWaifuRespawner> _respawner;
};
class SystemRespawnerFixture final : public IGuiPlugin
{
public:
    bool Initialize(GuiPluginContext& context, QString* error) override
    {
        auto page = std::make_unique<RespawnerPage>(*context.Services());
        if (!context.RegisterTab({"respawner", "respawner", 0}, page.get(), error)) return false;
        _page = page.release(); return true;
    }
    void Stop() noexcept override { if (_page) _page->stop(); }
private:
    RespawnerPage* _page = nullptr;
};
DESKWAIFU_EXPORT_GUI_PLUGIN(SystemRespawnerFixture)
#include "SystemRespawnerFixture.moc"
