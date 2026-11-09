// 定义 GUI 插件作用域的服务访问与页面注册上下文。
#pragma once

#include "GuiHostServices.hpp"
#include <QPointer>
#include <QSet>
#include <QWidget>
#include <memory>
#include <vector>

class QTabWidget;
class GuiPluginManager;

// 插件页面的显示信息；宿主先按 order 升序、再按全局 ID 排序。
struct GuiTabSpec
{
    QString id; // Plugin-local ID; the global ID is Name() + '/' + id.
    QString title;
    int order = 0;
};

// 仅在 GUI 主线程使用，存活至插件析构结束。
// 注册成功的页面由上下文拥有，Stop 后、插件析构前销毁；
// 注册失败时所有权仍归调用者。失效时撤销服务和受管理接收者的排队回调。
class GuiPluginContext final : public QObject
{
public:
    ~GuiPluginContext() override;
    const QString& Name() const { return _name; }
    // 返回上下文拥有的服务门面，不得删除；初始化时可登记传输，激活后才可提交命令。
    GuiHostServices* Services() const;
    // 返回是否尚未失效；有效不代表服务已激活或页面仍可登记。
    bool IsValid() const { return _valid; }
    // 在页面发布前登记无父对象的 GUI 线程页面，成功接管所有权并返回 true。
    // ID 须非空且不含斜杠，标题须非空；重复页面、重复 ID 等错误返回 false。
    // 提供 error 时，成功清空，失败写入原因；失败不接管 page。
    bool RegisterTab(const GuiTabSpec& spec, QWidget* page, QString* error = nullptr);

private:
    friend class GuiPluginManager;
    GuiPluginContext(QString name, GuiHostServices& services, QTabWidget& tabs, QSet<QString>& ids, QSet<QWidget*>& pages);
    void Activate();
    void Invalidate();
    void DestroyPages();
    struct Tab { QString id, title; int order; QPointer<QWidget> page; };
    QString _name;
    std::unique_ptr<GuiHostServices> _services;
    QTabWidget& _tabs;
    QSet<QString>& _ids;
    QSet<QWidget*>& _registeredPages;
    std::vector<Tab> _pages;
    bool _valid = true, _published = false;
};
