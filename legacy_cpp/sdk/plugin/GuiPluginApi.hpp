// 定义 GUI 插件接口、ABI 函数表及模块入口导出辅助。
#pragma once

#include "IPlugin.hpp"
#include "PluginAbi.hpp"

class GuiPluginContext;

// GUI 插件入口接口，生命周期方法均在 GUI 主线程执行。
class IGuiPlugin : public IPlugin {
public:
    // 登记页面和传输等资源，此时服务尚未激活，不可发送命令。
    // 成功返回 true；失败返回 false 并可写入 error，宿主仍会调用 Stop。
    virtual bool Initialize(GuiPluginContext& context, QString* error) = 0;
};

constexpr std::uint32_t DeskWaifuGuiPluginAbiVersion = 2;
constexpr const char* DeskWaifuGuiPluginEntrySymbol = "DeskWaifuGuiPluginEntry";

// GUI 插件导出函数表；宿主验证大小、版本、Qt 版本及构建 ABI 后才调用工厂。
// create 交出实例，实例必须由同一表的 destroy 释放；表具有静态生命周期。
struct GuiPluginApi {
    std::uint32_t size;
    std::uint32_t version;
    std::uint32_t qtVersion;
    const char* buildAbi;
    IGuiPlugin* (*create)();
    void (*destroy)(IGuiPlugin*);
};
using GuiPluginEntry = const GuiPluginApi* (*)();

// 为默认可构造的插件类型提供静态函数表，保持分配与释放位于同一模块。
template <class T> static const GuiPluginApi* DeskWaifuGuiPluginApi()
{
    static const GuiPluginApi api { sizeof(GuiPluginApi), DeskWaifuGuiPluginAbiVersion, QT_VERSION, DeskWaifuPluginBuildAbi(), []() -> IGuiPlugin* { return new T; },
        [](IGuiPlugin* plugin) { delete plugin; } };
    return &api;
}

// 导出宿主按固定符号查找的 GUI 插件入口。
#define DESKWAIFU_EXPORT_GUI_PLUGIN(Type)                                                                                                                                          \
    extern "C" Q_DECL_EXPORT const GuiPluginApi* DeskWaifuGuiPluginEntry() { return DeskWaifuGuiPluginApi<Type>(); }
