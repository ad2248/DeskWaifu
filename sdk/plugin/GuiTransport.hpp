// 定义由插件提供、由宿主代理借用的 GUI 传输接口。
#pragma once

#include "GuiHostServices.hpp"

// 插件拥有的 GUI 线程传输接口，复用宿主的命令及连接信号，
// 使页面和外壳不依赖具体协议客户端。
// 代理仅借用对象；插件负责在 Stop 期间清理传输并维护其生命周期。
class IGuiTransport : public GuiHostServices {
public:
    using GuiHostServices::GuiHostServices;
};
