#pragma once

#include "GuiHostServices.hpp"

// Plugin-owned GUI-thread transport. Reuse the host's command/connection
// signals so pages and the shell never depend on a concrete protocol client.
// The broker borrows this object; the plugin retains ownership through Stop.
class IGuiTransport : public GuiHostServices
{
public:
    using GuiHostServices::GuiHostServices;
};
