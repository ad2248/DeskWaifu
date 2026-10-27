#pragma once

#include "GuiHostServices.hpp"
#include <QPointer>
#include <QSet>
#include <QWidget>
#include <memory>
#include <vector>

class QTabWidget;
class GuiPluginManager;

struct GuiTabSpec
{
    QString id; // Plugin-local ID; the global ID is Name() + '/' + id.
    QString title;
    int order = 0;
};

// Main-thread only. Lives through plugin destruction. Registered pages belong
// to this context, not the plugin, and are destroyed after Stop, before plugin
// destruction. A failed registration leaves ownership with the caller.
class GuiPluginContext final : public QObject
{
public:
    ~GuiPluginContext() override;
    const QString& Name() const { return _name; }
    GuiHostServices* Services() const;
    bool IsValid() const { return _valid; }
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
