#include "plugin/PluginApi.hpp"
#include "plugin/GuiPluginApi.hpp"
#include "plugin/PluginContext.hpp"
#include "plugin/GuiTransport.hpp"
#include "processor/Processor.hpp"
#include "processor/Events.hpp"
#include "contracts/ICommandService.hpp"
#include "contracts/InputSample.hpp"
#include "contracts/JsonHelpers.hpp"
#include "contracts/ModelExchange.hpp"
#include "contracts/RenderDevice.hpp"
#include "contracts/SpeechEvents.hpp"
#ifdef DESKWAIFU_TEST_GUI
#include "plugin/GuiPluginContext.hpp"
#include "contracts/ImageFile.hpp"
#endif
#include <QCoreApplication>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
class EndpointOnly : public IProcessorPlugin
{
public:
    bool Initialize(PluginContext&, QString*) override { return true; }
    void Stop() noexcept override { stopped = true; }
    bool stopped = false;
};
class GuiOnly : public IGuiPlugin
{
public:
    bool Initialize(GuiPluginContext&, QString*) override { return true; }
    void Stop() noexcept override {}
};
class LifecycleOnly : public IPlugin
{
public:
    void Stop() noexcept override {}
};
class Transport : public IGuiTransport
{
public:
    bool isConnected() const override { return false; }
    void sendCommand(const QString&, const QJsonObject&) override {}
    void cancelQueuedCommand(const QString&) override {}
};
class Consumer : public Processor
{
public:
    explicit Consumer(ProcessorEventLoop& loop) : Processor(loop) {}
    bool HandleEvent(const ProcessorEvent&) override { ++calls; return false; }
    int calls = 0;
};
static_assert(std::is_base_of<IPlugin, IProcessorPlugin>::value, "processor plugins share lifecycle");
static_assert(std::is_base_of<IPlugin, IGuiPlugin>::value, "GUI plugins share lifecycle");
static_assert(!std::is_base_of<IProcessorPlugin, IGuiPlugin>::value, "typed plugin interfaces stay separate");
static_assert(!std::is_abstract<EndpointOnly>::value, "endpoint-only plugins need no dummy event consumer");
static_assert(!std::is_abstract<Processor>::value, "Processor supplies default lifecycle and optional consumption");
static_assert(std::is_same<decltype(PluginApi::create), IProcessorPlugin* (*)()>::value, "factory returns typed processor API");
static_assert(noexcept(std::declval<IPlugin&>().Stop()), "common shutdown cannot throw");
static_assert(noexcept(std::declval<Processor&>().Stop()), "Processor shutdown cannot throw");
static_assert(std::is_base_of<GuiHostServices, IGuiTransport>::value, "GUI transports share the public services contract");
}

int main(int argc, char** argv)
try {
    QCoreApplication app(argc, argv);
    LifecycleOnly common;
    GuiOnly gui;
    EndpointOnly endpoint;
    Transport transport;
    Check(!transport.registerTransport(&transport, &transport), "base services do not register a concrete transport");
    Check(common.Start(nullptr) && gui.Start(nullptr) && endpoint.Start(nullptr), "all plugin kinds inherit default Start");
    Check(!endpoint.HandleEvent(ProcessorEvent{"unconsumed"}), "endpoint-only plugin does not consume direct events");
    const auto* api = DeskWaifuPluginApi<EndpointOnly>();
    auto* created = api->create();
    IPlugin* lifecycle = created;
    lifecycle->Stop();
    Check(static_cast<EndpointOnly*>(created)->stopped, "common lifecycle dispatches to typed plugin");
    api->destroy(created);

    ProcessorEventLoop loop;
    Consumer receiver(loop);
    {
        Processor sender(loop);
        Check(loop.Contains(&sender) && loop.Contains(&receiver), "Processor construction registers on shared loop");
        Check(loop.Post({"cancel-on-stop", &sender, &receiver}), "queue event owned by sender");
        sender.Stop();
        sender.Stop();
        Check(!loop.Contains(&sender) && !loop.Post({"stale", &sender, &receiver}), "Stop unregisters idempotently");
        Check(loop.Post({"unrelated", nullptr, &receiver}), "unrelated queued work survives Stop");
        for (int i = 0; i < 20; ++i) QCoreApplication::processEvents();
        Check(receiver.calls == 1, "Stop suppresses queued pointers before plugin destruction");
    }
    IProcessorPlugin* retired = nullptr;
    {
        Processor scoped(loop);
        retired = &scoped;
        Check(loop.Contains(retired), "scoped registration exists");
    }
    Check(!loop.Contains(retired), "destruction unregisters even if Stop was omitted");

    ConfigStore::Snapshot snapshot;
    std::string error;
    Check(ConfigStore::Parse("{\"name\":\"sdk\",\"enabled\":true,\"count\":3}", snapshot, error), "standalone configuration parser linked");
    Check(JsonGetString(snapshot, "name") == "sdk" && JsonGetBool(snapshot, "enabled") && JsonGetInt(snapshot, "count", 0) == 3,
          "shared JSON helpers available through contracts include root");
    const auto previous = snapshot.values;
    Check(!ConfigStore::Parse("{\"x\":1,\"x\":2}", snapshot, error) && snapshot.values == previous, "duplicate keys rejected without changing snapshot");
    QTemporaryDir directory;
    Check(directory.isValid(), "temporary configuration directory");
    QJsonObject object;
    QString detail;
    const auto path = directory.filePath("config.json");
    Check(WriteJsonObject(path, {{"sdk", true}}, &detail) && ReadJsonObject(path, object, &detail) && object["sdk"].toBool(),
          "standalone atomic configuration helpers linked");
    std::cout << "SDK typed lifecycle, shared foundation and ownership contracts passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
