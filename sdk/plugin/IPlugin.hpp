// 定义后端与 GUI 插件共享的启动和停止生命周期。
#pragma once

#include <QString>

// 由宿主管理的插件生命周期接口；生命周期回调在宿主所属线程执行。
// 后端插件使用事件循环线程，GUI 插件使用 GUI 主线程。
class IPlugin
{
public:
    virtual ~IPlugin() = default;
    // 所有成功初始化的插件上下文激活后调用，可开始收发消息或启动工作。
    // 成功返回 true；失败返回 false，并可通过 error 提供原因。
    virtual bool Start(QString* error) { Q_UNUSED(error); return true; }
    // 必须处理部分初始化状态；返回前停止并等待所有工作线程退出，且不得抛出异常。
    // 调用前上下文已失效，不得再提交新任务；上下文仍存活到插件析构结束。
    virtual void Stop() noexcept = 0;
};
