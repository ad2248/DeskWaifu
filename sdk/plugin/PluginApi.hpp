// 定义后端插件 ABI 函数表及模块入口导出辅助。
#pragma once

#include "IProcessorPlugin.hpp"
#include "PluginAbi.hpp"

constexpr std::uint32_t DeskWaifuPluginAbiVersion = 2;
constexpr const char* DeskWaifuPluginEntrySymbol = "DeskWaifuPluginEntry";

// 后端插件导出函数表；宿主验证大小、版本、Qt 版本及构建 ABI 后才调用工厂。
// create 交出实例，实例必须由同一表的 destroy 释放；表具有静态生命周期。
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

// 为默认可构造的插件类型提供静态函数表，保持分配与释放位于同一模块。
template<class T> static const PluginApi* DeskWaifuPluginApi()
{
    static const PluginApi api{sizeof(PluginApi), DeskWaifuPluginAbiVersion, QT_VERSION,
        DeskWaifuPluginBuildAbi(), []() -> IProcessorPlugin* { return new T; }, [](IProcessorPlugin* plugin) { delete plugin; }};
    return &api;
}

// 导出宿主按固定符号查找的后端插件入口。
#define DESKWAIFU_EXPORT_PLUGIN(Type) \
    extern "C" Q_DECL_EXPORT const PluginApi* DeskWaifuPluginEntry() \
    { return DeskWaifuPluginApi<Type>(); }
