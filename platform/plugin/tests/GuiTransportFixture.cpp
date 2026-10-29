#include "plugin/GuiPluginApi.hpp"
#include "plugin/GuiPluginContext.hpp"
#include "plugin/GuiTransport.hpp"
#include <QFile>

namespace {
void Log(const QString& text)
{
    QFile file(qEnvironmentVariable("DESKWAIFU_GUI_TRANSPORT_TEST_LOG"));
    if (file.open(QIODevice::WriteOnly | QIODevice::Append)) file.write((text + '\n').toUtf8());
}
class Transport : public IGuiTransport
{
public:
    bool isConnected() const override { return false; }
    void sendCommand(const QString&, const QJsonObject&) override {}
    void cancelQueuedCommand(const QString&) override {}
};
class Fixture : public IGuiPlugin
{
public:
    ~Fixture() override { Log("plugin:destroy"); }
    bool Initialize(GuiPluginContext& context, QString* error) override
    {
        _context = &context;
        _transport.reset(new Transport);
        auto* services = context.Services();
        QObject wrongOwner;
        if (services->registerTransport(nullptr, &context) ||
            services->registerTransport(_transport.get(), nullptr) ||
            services->registerTransport(_transport.get(), &wrongOwner)) {
            *error = "invalid transport ownership accepted";
            return false;
        }
        if (!services->registerTransport(_transport.get(), &context)) {
            *error = "transport registration failed";
            return false;
        }
        Transport other;
        if (services->registerTransport(_transport.get(), &context) || services->registerTransport(&other, &context)) {
            *error = "duplicate transport accepted";
            return false;
        }
        services->unregisterTransport(&other);
        services->unregisterTransport(nullptr);
        auto* page = new QWidget;
        if (!context.RegisterTab({"transport", "Transport"}, page, error)) { delete page; return false; }
        const auto observe = [this](const QString&, const QJsonObject&) {
            Log(_context->IsValid() ? "callback:active" : "bad-invalid-callback");
        };
        QObject::connect(services, &GuiHostServices::responseReceived, &context, observe);
        QObject::connect(services, &GuiHostServices::responseReceived, &context, observe, Qt::QueuedConnection);
        QObject::connect(services, &GuiHostServices::responseReceived, page, observe, Qt::QueuedConnection);
        QObject::connect(services, &GuiHostServices::responseReceived, new QObject(page), observe, Qt::QueuedConnection);
#ifdef FIXTURE_init_fail
        *error = "fixture Initialize failed";
        return false;
#else
        return true;
#endif
    }
    bool Start(QString* error) override
    {
#ifdef FIXTURE_release
        _context->Services()->unregisterTransport(_transport.get());
        if (!_context->Services()->registerTransport(_transport.get(), _context)) {
            *error = "explicit release did not allow replacement";
            return false;
        }
#elif defined(FIXTURE_destroyed)
        _transport.reset();
#elif defined(FIXTURE_start_fail)
        *error = "fixture Start failed";
        return false;
#endif
        Q_UNUSED(error);
        return true;
    }
    void Stop() noexcept override
    {
        Log("plugin:stop");
        Transport replacement;
        if (_context->IsValid() || _context->Services()->registerTransport(&replacement, _context))
            Log("bad-stop-registration");
        _context->Services()->unregisterTransport(_transport.get());
        _context->Services()->unregisterTransport(nullptr);
        _transport.reset();
    }
private:
    GuiPluginContext* _context = nullptr;
    std::unique_ptr<Transport> _transport;
};
}
DESKWAIFU_EXPORT_GUI_PLUGIN(Fixture)
