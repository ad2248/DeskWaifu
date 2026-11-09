#pragma once

#include "plugin/GuiPluginApi.hpp"
#include "plugin/GuiPluginContext.hpp"
#include <QStringList>

// The host services and tab widget must outlive the manager. Main-thread only.
class GuiPluginManager final
{
public:
    GuiPluginManager(QString root, GuiHostServices& services, QTabWidget& tabs);
    ~GuiPluginManager();
    bool Start(QString* error = nullptr);
    void Stop();
    QStringList LoadedPlugins() const;
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
