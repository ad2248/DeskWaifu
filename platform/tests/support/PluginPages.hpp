#pragma once
#include "plugin/GuiPluginManager.hpp"
#include <QDir>
#include <QFile>
#include <QTabWidget>
#include <QTemporaryDir>
#include <stdexcept>

// 从构建产物复制 GUI 插件到私有安装，拥有标签容器并通过真实管理器加载页面。
// 仅在 GUI 主线程使用；借用的 services 必须比本对象存活更久。
class PluginPages
{
public:
    // 安装可用的领域插件；start 为 false 时推迟启动，安装或启动失败则抛出异常。
    explicit PluginPages(GuiHostServices& services, bool start = true) : _services(services)
    {
        if (!_installation.isValid()) throw std::runtime_error("private GUI plugin installation");
        for (const auto& domain : {"system", "interaction", "model", "speech"}) {
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
    // 启动已安装插件；管理器返回失败时抛出带诊断的 runtime_error。
    void Start()
    {
        QString error;
        if (!_plugins->Start(&error)) throw std::runtime_error(error.toStdString());
    }
    // 按对象名称查找页面并返回借用引用；未找到时抛出 runtime_error。
    // 返回控件由插件上下文管理，有效期不超过本夹具的生命周期。
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
