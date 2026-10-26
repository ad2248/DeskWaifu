#include "plugin/GuiPluginApi.hpp"
#include "plugin/GuiPluginContext.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>

// A different plugin may legitimately drain events while joining its work.
// All previously stopped plugins must already have stopped their timers.
class ShutdownPump final : public IGuiPlugin
{
public:
    bool Initialize(GuiPluginContext&, QString*) override { return true; }
    void Stop() noexcept override
    {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 200) QCoreApplication::processEvents();
    }
};
DESKWAIFU_EXPORT_GUI_PLUGIN(ShutdownPump)
