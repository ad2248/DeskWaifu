#pragma once

#include "IPlugin.hpp"
#include "PluginAbi.hpp"

class GuiPluginContext;

class IGuiPlugin : public IPlugin
{
public:
    virtual bool Initialize(GuiPluginContext& context, QString* error) = 0;
};

constexpr std::uint32_t DeskWaifuGuiPluginAbiVersion = 2;
constexpr const char* DeskWaifuGuiPluginEntrySymbol = "DeskWaifuGuiPluginEntry";

struct GuiPluginApi
{
    std::uint32_t size;
    std::uint32_t version;
    std::uint32_t qtVersion;
    const char* buildAbi;
    IGuiPlugin* (*create)();
    void (*destroy)(IGuiPlugin*);
};
using GuiPluginEntry = const GuiPluginApi* (*)();

template<class T> static const GuiPluginApi* DeskWaifuGuiPluginApi()
{
    static const GuiPluginApi api{sizeof(GuiPluginApi), DeskWaifuGuiPluginAbiVersion, QT_VERSION,
        DeskWaifuPluginBuildAbi(), []() -> IGuiPlugin* { return new T; }, [](IGuiPlugin* plugin) { delete plugin; }};
    return &api;
}

#define DESKWAIFU_EXPORT_GUI_PLUGIN(Type) \
    extern "C" Q_DECL_EXPORT const GuiPluginApi* DeskWaifuGuiPluginEntry() \
    { return DeskWaifuGuiPluginApi<Type>(); }
