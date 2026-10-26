#include "plugin/GuiPluginApi.hpp"
#include "plugin/GuiPluginContext.hpp"
#include <QFile>
#include <QCoreApplication>
#include <stdexcept>

namespace {
void Log(const QString& text)
{
    QFile file(qEnvironmentVariable("DESKWAIFU_GUI_PLUGIN_TEST_LOG"));
    if (file.open(QIODevice::WriteOnly | QIODevice::Append)) file.write((text + '\n').toUtf8());
}
struct UnloadProbe { ~UnloadProbe() { Log("unload"); } } probe;
class Page : public QWidget
{
public:
    explicit Page(QString name) : _name(std::move(name)) { setObjectName(QStringLiteral("fixturePage")); }
    ~Page() override { Log(_name + ":page-destroy"); }
private:
    QString _name;
};
class Fixture : public IGuiPlugin
{
public:
    ~Fixture() override
    {
        Log(_context->Name() + ":destroy");
        if (_context->IsValid() || _context->Services()->isConnected()) Log("bad-destroy-active");
    }
    bool Initialize(GuiPluginContext& context, QString* error) override
    {
        _context = &context;
        const auto name = context.Name();
        Log(name + ":initialize");
        auto* services = context.Services();
        QObject::connect(services, &GuiHostServices::responseReceived, &context,
            [name](const QString&, const QJsonObject&) { Log(name + ":response"); });
        QObject::connect(services, &GuiHostServices::responseReceived, &context,
            [name](const QString&, const QJsonObject&) { Log(name + ":queued-response"); }, Qt::QueuedConnection);
        QObject::connect(services, &GuiHostServices::activationChanged, &context,
            [name](bool) { Log(name + ":activation"); });
        _page = new Page(name);
        QObject::connect(services, &GuiHostServices::responseReceived, _page,
            [name](const QString&, const QJsonObject&) { Log(name + ":queued-page-response"); }, Qt::QueuedConnection);
        auto* queuedChild = new QObject(_page);
        QObject::connect(services, &GuiHostServices::responseReceived, queuedChild,
            [name](const QString&, const QJsonObject&) { Log(name + ":queued-child-response"); }, Qt::QueuedConnection);
        const QString id = "first";
        if (!context.RegisterTab({id, "First", 10}, _page, error)) { delete _page; _page = nullptr; return false; }
#ifdef FIXTURE_validation
        auto* rejected = new QWidget;
        if (context.RegisterTab({id, "Duplicate"}, rejected) || rejected->parent()) throw std::runtime_error("duplicate accepted");
        delete rejected;
        if (context.RegisterTab({"reuse", "Reuse"}, _page) ||
            context.RegisterTab({"", "Empty"}, _page) || context.RegisterTab({"null", "Null"}, nullptr) ||
            context.RegisterTab({"bad/id", "Bad namespace"}, _page))
            throw std::runtime_error("invalid page accepted");
        QWidget parent;
        QWidget child(&parent);
        if (context.RegisterTab({"parent", "Parented"}, &child)) throw std::runtime_error("parented accepted");
#endif
        auto* second = new Page(name);
        const QString secondId =
#ifdef FIXTURE_duplicate
            "first";
#else
            "second";
#endif
        if (!context.RegisterTab({secondId, "Second", 0}, second, error)) { delete second; return false; }
#ifdef FIXTURE_init_fail
        *error = "fixture Initialize failed";
        return false;
#elif defined(FIXTURE_init_throw)
        throw std::runtime_error("fixture Initialize threw");
#else
        services->sendCommand("initialize", {{"plugin", name}});
        return true;
#endif
    }
    bool Start(QString* error) override
    {
        Log(_context->Name() + ":start");
        if (_page->parent()) { *error = "published before Start"; return false; }
#ifdef FIXTURE_start_fail
        *error = "fixture Start failed";
        return false;
#elif defined(FIXTURE_start_throw)
        throw std::runtime_error("fixture Start threw");
#else
        _context->Services()->setStatusMessage(_context->Name());
        _context->Services()->sendCommand("start");
        _context->Services()->publishLog("fixture log");
        _context->Services()->notifyBackendReady();
        if (_context->Services()->property("procDirectory").toString() != "/fixture/proc") {
            *error = "host metadata not copied";
            return false;
        }
        return true;
#endif
    }
    void Stop() noexcept override
    {
        Log(_context->Name() + ":stop");
        QCoreApplication::processEvents(); // Exercise reentrant teardown after invalidation.
        if (_context->IsValid() || _context->Services()->isConnected()) Log("bad-stop-active");
        _context->Services()->sendCommand("bad-after-stop");
        _context->Services()->cancelQueuedCommand("bad-after-stop");
        _context->Services()->setStatusMessage("bad-after-stop");
    }
private:
    GuiPluginContext* _context = nullptr;
    QWidget* _page = nullptr;
};
}

#if defined(FIXTURE_bad_abi) || defined(FIXTURE_bad_build) || defined(FIXTURE_bad_qt)
extern "C" Q_DECL_EXPORT const GuiPluginApi* DeskWaifuGuiPluginEntry()
{
    static GuiPluginApi api = *DeskWaifuGuiPluginApi<Fixture>();
#ifdef FIXTURE_bad_abi
    api.version = 99;
#elif defined(FIXTURE_bad_build)
    api.buildAbi = "wrong";
#else
    api.qtVersion = 0;
#endif
    return &api;
}
#elif defined(FIXTURE_factory_throw)
extern "C" Q_DECL_EXPORT const GuiPluginApi* DeskWaifuGuiPluginEntry()
{
    static GuiPluginApi api = *DeskWaifuGuiPluginApi<Fixture>();
    api.create = []() -> IGuiPlugin* { throw std::runtime_error("factory"); };
    return &api;
}
#elif !defined(FIXTURE_backend_only)
DESKWAIFU_EXPORT_GUI_PLUGIN(Fixture)
#endif

#if defined(FIXTURE_dual) || defined(FIXTURE_backend_only)
class Backend : public IPlugin
{
public:
    bool Initialize(PluginContext&, QString*) override { Log("backend:initialize"); return true; }
    void Stop() noexcept override { Log("backend:stop"); }
};
DESKWAIFU_EXPORT_PLUGIN(Backend)
#endif
