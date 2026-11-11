#pragma once

#include "plugin/PluginApi.hpp"
#include "plugin/PluginContext.hpp"
#include <QStringList>

// 管理后端插件及其上下文，借用的 loop 必须比管理器存活更久。
// 生命周期操作须在事件循环所属线程进行；动态库始终保持映射，不随实例销毁卸载。
// Loop-thread-only. Failed plugins are rolled back individually; healthy ones
// remain available. Start returns false with diagnostics on partial failure.
class PluginManager final {
public:
    PluginManager(ProcessorEventLoop& loop, QString pluginRoot, QString configRoot);
    // 在事件循环所属线程调用 Stop，清理仍保留的实例。
    ~PluginManager();
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;
    // 全部插件成功时返回 true；失败插件单独回滚，健康实例仍可用。
    // 可选 error 接收诊断；已启动时返回 false，重新启动前须先 Stop。
    bool Start(QString* error = nullptr);
    // 先使所有端点失效，再逆序停止并销毁插件；不再执行插件拥有的完成回调。
    // 清理失败（包括启动回滚遗留的失败）时返回 false，并写入可选 error。
    bool Stop(QString* error = nullptr);
    // 返回当前保留的插件名称；调用方须遵守事件循环线程约束。
    QStringList LoadedPlugins() const;
    // 按名称排序返回绝对路径，排除符号链接；目录缺失或为空时返回空列表。
    static QStringList Discover(const QString& root); // Exact <root>/<name>/<name>.so, no recursion.

private:
    struct Instance {
        QString name;
        const PluginApi* api = nullptr;
        IProcessorPlugin* plugin = nullptr;
        std::unique_ptr<PluginContext> context;
    };
    ProcessorEventLoop& _loop;
    QString _pluginRoot, _configRoot;
    std::vector<Instance> _instances;
    // Rejected instances are already gone, but Stop must still report their cleanup failure.
    QStringList _pendingShutdownFailures;
    bool _started = false;
};
