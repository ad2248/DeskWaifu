#pragma once

#include "IProcessorPlugin.hpp"
#include "PluginAbi.hpp"

constexpr std::uint32_t DeskWaifuPluginAbiVersion = 2;
constexpr const char* DeskWaifuPluginEntrySymbol = "DeskWaifuPluginEntry";

struct PluginApi
{
    std::uint32_t size;
    std::uint32_t version;
    std::uint32_t qtVersion;
    const char* buildAbi;
    IProcessorPlugin* (*create)();
    void (*destroy)(IProcessorPlugin*);
};
using PluginEntry = const PluginApi* (*)();

template<class T> static const PluginApi* DeskWaifuPluginApi()
{
    static const PluginApi api{sizeof(PluginApi), DeskWaifuPluginAbiVersion, QT_VERSION,
        DeskWaifuPluginBuildAbi(), []() -> IProcessorPlugin* { return new T; }, [](IProcessorPlugin* plugin) { delete plugin; }};
    return &api;
}

#define DESKWAIFU_EXPORT_PLUGIN(Type) \
    extern "C" Q_DECL_EXPORT const PluginApi* DeskWaifuPluginEntry() \
    { return DeskWaifuPluginApi<Type>(); }
