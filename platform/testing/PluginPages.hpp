#pragma once
#include "plugin/GuiPluginManager.hpp"
#include <QDir>
#include <QFile>
#include <QTabWidget>
#include <QTemporaryDir>
#include <stdexcept>

class PluginPages
{
public:
    explicit PluginPages(GuiHostServices& services, bool start = true) : _services(services)
    {
        if (!_installation.isValid()) throw std::runtime_error("private GUI plugin installation");
        for (const auto& domain : {"system", "interaction", "model", "log", "speech"}) {
            const QDir source(QStringLiteral(GUI_PLUGIN_ROOT) + '/' + QString::fromLatin1(domain));
            if (!source.exists()) continue;
            const auto destination = _installation.path() + '/' + QString::fromLatin1(domain);
            if (!QDir().mkpath(destination)) throw std::runtime_error("GUI plugin directory");
            for (const auto& file : source.entryList({"*.so"}, QDir::Files))
                if (!QFile::copy(source.filePath(file), destination + '/' + file)) throw std::runtime_error("copy GUI plugin");
        }
        _plugins.reset(new GuiPluginManager(_installation.path(), _services, _tabs));
        if (start) Start();
    }
    void Start()
    {
        QString error;
        if (!_plugins->Start(&error)) throw std::runtime_error(error.toStdString());
    }
    QWidget& Page(const QString& name)
    {
        auto* page = _tabs.findChild<QWidget*>(name);
        if (!page) throw std::runtime_error(("missing GUI page: " + name).toStdString());
        return *page;
    }
private:
    GuiHostServices& _services;
    QTemporaryDir _installation;
    QTabWidget _tabs;
    std::unique_ptr<GuiPluginManager> _plugins;
};
