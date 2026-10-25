#include "plugin/PluginApi.hpp"
#include "plugin/PluginContext.hpp"
#include <QFile>

namespace {
void Log(const char* text)
{
    QFile file(qEnvironmentVariable("DESKWAIFU_PLUGIN_TEST_LOG"));
    if (file.open(QIODevice::WriteOnly | QIODevice::Append)) { file.write(text); file.write("\n"); }
}

struct ModuleLifetime { ~ModuleLifetime() { Log("unload"); } } moduleLifetime;
struct ModuleEvent : ProcessorEvent
{
    ModuleEvent() : ProcessorEvent("fixture-from-plugin") {}
    ~ModuleEvent() override { Log("payload-destroy"); }
};

class FixturePlugin : public IPlugin
{
public:
    ~FixturePlugin() override { Log("destroy"); }
    bool Initialize(PluginContext& context, QString* error) override
    {
        Log("initialize");
        _context = &context;
        _endpoint = context.CreateEndpoint("service", [this](const EventDelivery& message) {
            if (message.kind == EventDelivery::Kind::Request) {
                Log("request");
                _endpoint->Respond(message.requestId, message.payload);
            }
        });
        if (!_endpoint) return false;
#ifndef FIXTURE_consumer
        _endpoint->Provide("fixture.compute");
#endif
        if (_endpoint->Notify("during-initialize", std::make_shared<ProcessorEvent>())) return false;
        if (!context.WriteConfig("settings.json", {{"plugin", context.Name()}}, error)) return false;
#ifdef FIXTURE_init_fail
        if (error) *error = "intentional initialization failure";
        return false;
#else
        return true;
#endif
    }
    bool Start(QString* error) override
    {
        Log("start");
        _endpoint->Notify("fixture.started", std::make_shared<ModuleEvent>());
#ifdef FIXTURE_start_fail
        if (error) *error = "intentional start failure";
        return false;
#endif
#ifdef FIXTURE_consumer
        const auto id = _endpoint->Request("fixture.compute", std::make_shared<ProcessorEvent>(),
            [](const RequestResult& result) { Log(result.status == RequestStatus::Replied ? "replied" : "failed"); });
        return id != 0;
#else
        Q_UNUSED(error);
        return true;
#endif
    }
    void Stop() noexcept override
    {
        Log("stop");
        // Context remains usable through Stop, but messaging is disabled.
        if (_endpoint && _endpoint->Notify("during-stop", std::make_shared<ProcessorEvent>())) Log("bad-stop-active");
    }
private:
    PluginContext* _context = nullptr;
    EventEndpoint* _endpoint = nullptr;
};
}

#if defined(FIXTURE_bad_entry)
extern "C" Q_DECL_EXPORT int NotThePluginEntry() { return 1; }
#elif defined(FIXTURE_bad_abi) || defined(FIXTURE_bad_build_abi)
extern "C" Q_DECL_EXPORT const PluginApi* DeskWaifuPluginEntry()
{
    static PluginApi api = *DeskWaifuPluginApi<FixturePlugin>();
#ifdef FIXTURE_bad_abi
    api.version = DeskWaifuPluginAbiVersion + 1;
#else
    api.buildAbi = "deliberately incompatible compiler ABI";
#endif
    return &api;
}
#else
DESKWAIFU_EXPORT_PLUGIN(FixturePlugin)
#endif
