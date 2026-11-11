#pragma once

#include "plugin/GuiPluginApi.hpp"
#include "plugin/GuiPluginContext.hpp"
#include <QStringList>

// 管理 GUI 插件的初始化、启动及页面发布，失败插件单独回滚，不影响健康插件。
// 宿主服务和标签容器必须比管理器存活更久；生命周期操作仅限 GUI 主线程。
// 动态库保持映射，Stop 只销毁插件实例及其页面，不卸载库。
// The host services and tab widget must outlive the manager. Main-thread only.
class GuiPluginManager final {
public:
    GuiPluginManager(QString root, GuiHostServices& services, QTabWidget& tabs);
    // 析构时调用 Stop，须保持 GUI 应用及借用对象有效。
    ~GuiPluginManager();
    // 全部插件成功时返回 true；部分失败也会保留成功实例，诊断写入可选 error。
    // 已启动时返回 false；重新启动前须先 Stop。
    bool Start(QString* error = nullptr);
    // 先使全部上下文失效，再逆序停止插件并销毁页面和实例；可重复调用。
    void Stop();
    // 返回当前保留的插件名称，须在 GUI 主线程调用。
    QStringList LoadedPlugins() const;
    // 按名称排序返回 <root>/<name>/<name>.so 的绝对路径，不递归且排除符号链接。
    static QStringList Discover(const QString& root);

private:
    struct Instance {
        QString name;
        const GuiPluginApi* api = nullptr;
        std::unique_ptr<GuiPluginContext> context;
        IGuiPlugin* plugin = nullptr;
    };
    static void Discard(Instance& instance);
    QString _root;
    GuiHostServices& _services;
    QTabWidget& _tabs;
    QSet<QString> _ids;
    QSet<QWidget*> _pages;
    std::vector<Instance> _instances;
    bool _started = false;
};
