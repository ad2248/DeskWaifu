#include "PluginManager.hpp"
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QThread>
#include <cstring>

PluginManager::PluginManager(ProcessorEventLoop& loop, QString pluginRoot, QString configRoot)
    : _loop(loop), _pluginRoot(std::move(pluginRoot)), _configRoot(std::move(configRoot)) {}
PluginManager::~PluginManager() { Stop(); }

QStringList PluginManager::Discover(const QString& root)
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

bool PluginManager::Start(QString* error)
{
    if (error) error->clear();
    if (QThread::currentThread() != _loop.thread()) {
        if (error) *error = QStringLiteral("PluginManager must run on the event-loop thread");
        return false;
    }
    if (_started) { if (error) *error = QStringLiteral("Plugins already started"); return false; }
    QStringList failures;
    const auto discard = [&](Instance& instance) {
        instance.context->Invalidate();
        instance.plugin->Stop();
        instance.api->destroy(instance.plugin);
        instance.plugin = nullptr;
        if (!instance.context->_failure.isEmpty()) {
            const auto failure = instance.name + ": cleanup failed: " + instance.context->_failure;
            failures.append(failure);
            _pendingShutdownFailures.append(failure);
        }
    };
    // A missing optional plugin directory simply means there are no plugins.
    for (const auto& path : Discover(_pluginRoot)) {
        QLibrary library(path);
        // Keep even rejected libraries mapped: entry/static initialization may
        // have registered Qt metatypes whose destructors live in the module.
        library.setLoadHints(QLibrary::ResolveAllSymbolsHint | QLibrary::PreventUnloadHint);
        if (!library.load()) { failures.append(path + ": " + library.errorString()); continue; }
        const auto entry = reinterpret_cast<PluginEntry>(library.resolve(DeskWaifuPluginEntrySymbol));
        if (!entry) continue; // GUI-only modules are normal in the shared plugin directory.
        const PluginApi* api = nullptr;
        try { api = entry(); } catch (...) { failures.append(path + ": plugin entry threw"); continue; }
        if (!api || api->size != sizeof(PluginApi) || api->version != DeskWaifuPluginAbiVersion ||
            api->qtVersion != QT_VERSION || !api->buildAbi ||
            std::strcmp(api->buildAbi, DeskWaifuPluginBuildAbi()) != 0 || !api->create || !api->destroy)
            { failures.append(path + ": incompatible plugin ABI"); continue; }
        Instance instance;
        instance.name = QFileInfo(path).completeBaseName();
        instance.api = api;
        instance.context.reset(new PluginContext(_loop, instance.name,
            QDir(_configRoot).filePath("plugin/" + instance.name)));
        try { instance.plugin = api->create(); } catch (...) { failures.append(path + ": plugin factory threw"); continue; }
        if (!instance.plugin) { failures.append(path + ": plugin factory returned null"); continue; }
        QString detail;
        bool initialized = false;
        try { initialized = instance.plugin->Initialize(*instance.context, &detail); }
        catch (...) { detail = QStringLiteral("Initialize threw"); }
        if (!initialized) { failures.append(path + ": " + detail); discard(instance); continue; }
        _instances.push_back(std::move(instance));
    }
    // No plugin can send/receive until all initializers have completed.
    for (auto& instance : _instances) instance.context->Activate();
    for (auto it = _instances.begin(); it != _instances.end();) {
        auto& instance = *it;
        QString detail;
        bool started = false;
        try { started = instance.plugin->Start(&detail); }
        catch (...) { detail = QStringLiteral("Start threw"); }
        if (!started) {
            failures.append(instance.name + ": " + detail);
            discard(instance);
            it = _instances.erase(it);
        } else ++it;
    }
    _started = true;
    if (error) *error = failures.join('\n');
    return failures.isEmpty();
}

bool PluginManager::Stop(QString* error)
{
    Q_ASSERT(QThread::currentThread() == _loop.thread());
    if (error) error->clear();
    QStringList failures;
    failures.swap(_pendingShutdownFailures);
    // Invalidate every endpoint before any plugin stops. Completion callbacks
    // owned by plugins can no longer run; external requesters get unavailable.
    for (auto& instance : _instances) instance.context->Invalidate();
    for (auto it = _instances.rbegin(); it != _instances.rend(); ++it) it->plugin->Stop();
    for (auto it = _instances.rbegin(); it != _instances.rend(); ++it) {
        it->api->destroy(it->plugin);
        it->plugin = nullptr;
        if (!it->context->_failure.isEmpty()) failures.append(it->name + ": " + it->context->_failure);
    }
    _instances.clear();
    _started = false;
    if (error) *error = failures.join('\n');
    return failures.isEmpty();
}

QStringList PluginManager::LoadedPlugins() const
{
    QStringList result;
    for (const auto& instance : _instances) result.append(instance.name);
    return result;
}
