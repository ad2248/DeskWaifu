#pragma once
#include "event/ProcessorEventLoop.hpp"
#include <QStringList>
#include <memory>

// 将指定后端模块复制到私有临时安装并管理其生命周期，避免测试共享安装目录。
// 借用的事件循环须比本对象存活更久；构造、停止及析构须在循环所属线程执行。
class LoadedBackendPlugins {
public:
    // 按列表顺序安装模块；安装、启动失败或实际加载数不匹配时抛出 runtime_error。
    LoadedBackendPlugins(ProcessorEventLoop& loop, const QStringList& modules);
    // 停止插件后清理临时安装。
    ~LoadedBackendPlugins();
    // 提前停止插件，可重复调用；此测试封装不返回管理器的清理诊断。
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
