#include "GuiPluginManager.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QTabWidget>
#include <QThread>
#include <cstring>
#include <algorithm>

GuiPluginManager::GuiPluginManager(QString root, GuiHostServices& services, QTabWidget& tabs)
    : _root(std::move(root)), _services(services), _tabs(tabs) {}
GuiPluginManager::~GuiPluginManager() { Stop(); }

QStringList GuiPluginManager::Discover(const QString& root)
{
    QStringList paths;
    if (root.isEmpty()) return paths;
    const QDir directory(root);
    const auto names = directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
    for (const auto& name : names) {
        const QFileInfo file(directory.filePath(name + '/' + name + ".so"));
        if (file.isFile() && !file.isSymLink()) paths.append(file.absoluteFilePath());
    }
    return paths;
}

void GuiPluginManager::Discard(Instance& instance)
{
    instance.context->Invalidate();
    instance.plugin->Stop();
    instance.context->DestroyPages();
    instance.api->destroy(instance.plugin);
    instance.plugin = nullptr;
}

bool GuiPluginManager::Start(QString* error)
{
    if (error) error->clear();
    if (!QCoreApplication::instance() || QThread::currentThread() != QCoreApplication::instance()->thread() ||
        _services.thread() != QThread::currentThread() || _tabs.thread() != QThread::currentThread()) {
        if (error) *error = QStringLiteral("GuiPluginManager must run on the GUI main thread");
        return false;
    }
    if (_started) { if (error) *error = QStringLiteral("GUI plugins already started"); return false; }
    QStringList failures;
    for (const auto& path : Discover(_root)) {
        QLibrary library(path);
        library.setLoadHints(QLibrary::ResolveAllSymbolsHint | QLibrary::PreventUnloadHint);
        if (!library.load()) { failures.append(path + ": " + library.errorString()); continue; }
        const auto entry = reinterpret_cast<GuiPluginEntry>(library.resolve(DeskWaifuGuiPluginEntrySymbol));
        if (!entry) continue; // Backend-only modules are normal.
        const GuiPluginApi* api = nullptr;
        try { api = entry(); } catch (...) { failures.append(path + ": GUI plugin entry threw"); continue; }
        if (!api || api->size != sizeof(GuiPluginApi) || api->version != DeskWaifuGuiPluginAbiVersion ||
            api->qtVersion != QT_VERSION || !api->buildAbi ||
            std::strcmp(api->buildAbi, DeskWaifuPluginBuildAbi()) != 0 || !api->create || !api->destroy)
            { failures.append(path + ": incompatible GUI plugin ABI"); continue; }
        Instance instance;
        instance.name = QFileInfo(path).completeBaseName();
        instance.api = api;
        instance.context.reset(new GuiPluginContext(instance.name, _services, _tabs, _ids, _pages));
        try { instance.plugin = api->create(); } catch (...) { failures.append(path + ": GUI plugin factory threw"); continue; }
        if (!instance.plugin) { failures.append(path + ": GUI plugin factory returned null"); continue; }
        QString detail;
        bool initialized = false;
        try { initialized = instance.plugin->Initialize(*instance.context, &detail); }
        catch (...) { detail = QStringLiteral("Initialize threw"); }
        if (!initialized) { failures.append(path + ": " + detail); Discard(instance); continue; }
        _instances.push_back(std::move(instance));
    }
    for (auto& instance : _instances) instance.context->Activate();
    for (auto it = _instances.begin(); it != _instances.end();) {
        QString detail;
        bool started = false;
        try { started = it->plugin->Start(&detail); }
        catch (...) { detail = QStringLiteral("Start threw"); }
        if (!started) {
            failures.append(it->name + ": " + detail);
            Discard(*it);
            it = _instances.erase(it);
        } else ++it;
    }
    std::vector<GuiPluginContext::Tab*> pages;
    for (auto& instance : _instances) {
        instance.context->_published = true;
        for (auto& page : instance.context->_pages) pages.push_back(&page);
    }
    std::sort(pages.begin(), pages.end(), [](const GuiPluginContext::Tab* a, const GuiPluginContext::Tab* b) {
        return a->order != b->order ? a->order < b->order : a->id < b->id;
    });
    for (const auto* page : pages) if (page->page) {
        page->page->setProperty("deskwaifuTabId", page->id);
        _tabs.addTab(page->page, page->title);
    }
    _started = true;
    if (error) *error = failures.join('\n');
    return failures.isEmpty();
}

void GuiPluginManager::Stop()
{
    Q_ASSERT(QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread());
    for (auto& instance : _instances) instance.context->Invalidate();
    for (auto it = _instances.rbegin(); it != _instances.rend(); ++it) it->plugin->Stop();
    for (auto it = _instances.rbegin(); it != _instances.rend(); ++it) {
        it->context->DestroyPages();
        it->api->destroy(it->plugin);
        it->plugin = nullptr;
    }
    _instances.clear();
    _started = false;
}

QStringList GuiPluginManager::LoadedPlugins() const
{
    Q_ASSERT(QCoreApplication::instance() && QThread::currentThread() == QCoreApplication::instance()->thread());
    QStringList names;
    for (const auto& instance : _instances) names.append(instance.name);
    return names;
}
