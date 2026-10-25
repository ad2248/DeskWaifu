#include "plugin/PluginApi.hpp"
#include "plugin/PluginContext.hpp"
#include "processor/reading_clip/ReadingClipProcessor.hpp"

class ReadingPlugin final : public IPlugin
{
public:
    bool Initialize(PluginContext& context, QString*) override
    {
        auto* endpoint = context.CreateEndpoint("reading", [this](const EventDelivery& delivery) {
            if (_reading) _reading->HandleDelivery(delivery);
        });
        if (!endpoint || !endpoint->Provide("command.reading_clip/v1") ||
            !endpoint->Provide("command.get_reading_clip_status/v1")) return false;
        _reading.reset(new ReadingClipProcessor(context.Loop(), *endpoint));
        return true;
    }
    void Stop() noexcept override { if (_reading) _reading->Stop(); _reading.reset(); }
private:
    std::unique_ptr<ReadingClipProcessor> _reading;
};

DESKWAIFU_EXPORT_PLUGIN(ReadingPlugin)
