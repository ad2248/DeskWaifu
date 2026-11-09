#pragma once

#include "IPlugin.hpp"

class PluginContext;
struct ProcessorEvent;

class IProcessorPlugin : public IPlugin
{
public:
    // Register endpoints and read configuration only. No cross-plugin calls yet.
    virtual bool Initialize(PluginContext& context, QString* error) = 0;
    // Event consumption is optional: endpoint-only plugins use context-owned
    // handlers instead and must not need a dummy legacy consumer implementation.
    virtual bool HandleEvent(const ProcessorEvent& event) { Q_UNUSED(event); return false; }
};
