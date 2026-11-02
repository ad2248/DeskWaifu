#include "plugin/PluginApi.hpp"
#include "plugin/PluginContext.hpp"
#include "processor/command/CommandProcessor.hpp"
#include "processor/ipc/IpcProcessor.hpp"
#include <memory>

class IpcPlugin final : public IProcessorPlugin
{
public:
    bool Initialize(PluginContext& context, QString* error) override
    {
        if (error) error->clear();
        _commands = std::make_unique<CommandProcessor>(context.Loop(), &context);
        _ipc = std::make_unique<IpcProcessor>(context.Loop(), std::make_unique<LAppIPC>(), *_commands);
        return true;
    }
    bool Start(QString* error) override
    {
        if (error) error->clear();
        if (_ipc && _ipc->Start()) return true;
        if (error) *error = QStringLiteral("Cannot start IPC transport");
        return false;
    }
    void Stop() noexcept override
    {
        if (_ipc) _ipc->Stop();
        if (_commands) _commands->Stop();
        _ipc.reset();
        _commands.reset();
    }
private:
    std::unique_ptr<CommandProcessor> _commands;
    std::unique_ptr<IpcProcessor> _ipc;
};

DESKWAIFU_EXPORT_PLUGIN(IpcPlugin)
