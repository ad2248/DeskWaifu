// 定义后端插件初始化与可选的旧式事件处理接口。
#pragma once

#include "IPlugin.hpp"

class PluginContext;
struct ProcessorEvent;

// 后端插件接口，支持上下文端点及可选的旧式事件消费。
class IProcessorPlugin : public IPlugin {
public:
    // 在事件循环所属线程登记端点、读取配置，此阶段不得跨插件调用。
    // 成功返回 true；失败返回 false，并可写入 error；失败后仍会调用 Stop。
    virtual bool Initialize(PluginContext& context, QString* error) = 0;
    // 在事件循环所属线程处理旧式事件，返回 true 表示消费并停止后续派发。
    // event 仅在回调期间借用；仅使用端点的插件无需覆盖，默认不消费。
    virtual bool HandleEvent(const ProcessorEvent& event)
    {
        Q_UNUSED(event);
        return false;
    }
};
