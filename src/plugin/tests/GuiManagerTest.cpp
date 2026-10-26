#include "plugin/GuiPluginManager.hpp"
#include "plugin/PluginManager.hpp"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLibrary>
#include <QTabWidget>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
QByteArray Read(const QString& path) { QFile file(path); Check(file.open(QIODevice::ReadOnly), "read log"); return file.readAll(); }
void Clear(const QString& path) { QFile file(path); Check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "clear log"); }
void Copy(const QString& source, const QString& root, const QString& name)
{
    Check(QDir().mkpath(root + '/' + name), "make directory");
    Check(QFile::copy(source, root + '/' + name + '/' + name + ".so"), "copy fixture");
}
class Host : public GuiHostServices
{
public:
    bool isConnected() const override { return true; }
    void sendCommand(const QString& command, const QJsonObject&) override { commands.append(command); }
    void cancelQueuedCommand(const QString& command) override { commands.append(command); }
    void setStatusMessage(const QString& message, int) override { statuses.append(message); }
    QStringList commands, statuses;
};
}

int main(int argc, char** argv)
try {
    QApplication app(argc, argv);
    Check(argc == 14, "fixture paths");
    QTemporaryDir directory;
    Check(directory.isValid(), "temporary directory");
    const auto log = directory.path() + "/lifecycle.log";
    qputenv("DESKWAIFU_GUI_PLUGIN_TEST_LOG", log.toUtf8());
    Clear(log);
    const auto root = directory.path() + "/plugins";
    Copy(argv[1], root, "a-good");
    Copy(argv[2], root, "b-dual");
    Copy(argv[3], root, "c-backend");
    Host host;
    host.setProperty("procDirectory", "/fixture/proc");
    int logs = 0, ready = 0;
    QObject::connect(&host, &GuiHostServices::logMessage, &host, [&](const QString&) { ++logs; });
    QObject::connect(&host, &GuiHostServices::backendReady, &host, [&] { ++ready; });
    QTabWidget tabs;
    GuiPluginManager manager(root, host, tabs);
    QString error;
    Check(manager.Start(&error), qPrintable(error));
    Check(manager.LoadedPlugins() == QStringList{"a-good", "b-dual"} && tabs.count() == 4, "GUI-only and dual entries, multiple tabs; backend-only skipped");
    Check(tabs.widget(0)->property("deskwaifuTabId") == "a-good/second" && tabs.widget(1)->property("deskwaifuTabId") == "b-dual/second" &&
          tabs.widget(2)->property("deskwaifuTabId") == "a-good/first" && tabs.widget(3)->property("deskwaifuTabId") == "b-dual/first", "tabs sorted by order then namespaced ID");
    Check(tabs.widget(0)->objectName() == "fixturePage", "host preserves page objectName for UI lookup and styling");
    Check(!host.commands.contains("initialize") && host.commands.count("start") == 2, "IPC sends prohibited until activation");
    Check(logs == 2 && ready == 2, "log publication and backend-ready forward once without echo recursion");
    Check(!manager.Start(&error) && tabs.count() == 4, "duplicate Start non-destructive");
    emit host.responseReceived("test", {});
    emit host.activationChanged(false);
    Check(Read(log).contains("a-good:response") && Read(log).contains("b-dual:activation"), "shared IPC and process-level activation forwarded");
    const auto commands = host.commands;
    const auto statuses = host.statuses;
    manager.Stop();
    app.processEvents();
    emit host.responseReceived("late", {});
    Check(tabs.count() == 0 && manager.LoadedPlugins().isEmpty(), "stop removes pages");
    const auto stopped = Read(log);
    Check(!stopped.contains("bad-") && !stopped.contains("queued-response"), "invalidation disables services and queued context callbacks before Stop");
    Check(stopped.count("a-good:response") == 1 && host.commands == commands && host.statuses == statuses, "no callback or command after invalidation");
    Check(stopped.indexOf("a-good:stop") < stopped.indexOf("a-good:page-destroy") &&
          stopped.lastIndexOf("a-good:page-destroy") < stopped.indexOf("a-good:destroy"), "Stop then pages then plugin, context outlives plugin");
    QLibrary probe(root + "/a-good/a-good.so");
    Check(probe.load(), "probe mapped module");
    probe.unload();
    Check(!Read(log).contains("unload"), "PreventUnload retains plugin code");
    Check(manager.Start(&error) && tabs.count() == 4, "restart without leaked IDs");
    manager.Stop();
    {
        ProcessorEventLoop loop;
        PluginManager backend(loop, root, directory.path() + "/config");
        Check(backend.Start(&error), qPrintable(error));
        Check(backend.LoadedPlugins() == QStringList{"b-dual", "c-backend"}, "backend skips GUI-only entry and accepts dual module");
        backend.Stop();
    }
    for (int i = 4; i < argc; ++i) {
        Clear(log);
        const auto failureRoot = directory.path() + "/case-" + QString::number(i);
        Copy(argv[1], failureRoot, "a-good");
        Copy(argv[i], failureRoot, "z-bad");
        GuiPluginManager failing(failureRoot, host, tabs);
        const bool success = failing.Start(&error);
        const bool validation = i == 13;
        Check(success == validation, "failure status or successful registration validation");
        Check(validation || !error.isEmpty(), "failure has diagnostic");
        Check(tabs.count() == (validation ? 4 : 2), "per-plugin rollback leaves healthy tabs");
        const auto events = Read(log);
        if (i >= 4 && i <= 7) {
            Check(events.contains("z-bad:stop") && events.count("z-bad:page-destroy") == 2 && events.contains("z-bad:destroy"), "partial Initialize and Start failures clean all owned pages");
        }
        if (i == 8) Check(events.count("z-bad:page-destroy") == 2 && events.contains("z-bad:destroy"), "duplicate local ID rejected without ownership transfer");
        failing.Stop();
        Check(tabs.count() == 0 && !Read(log).contains("bad-"), "failure lifecycle clean");
    }
    GuiPluginManager missing(directory.path() + "/missing", host, tabs);
    Check(missing.Start(&error) && error.isEmpty(), "missing plugin directory is normal");
    missing.Stop();
    bool workerResult = true;
    std::thread worker([&] { workerResult = manager.Start(&error); });
    worker.join();
    Check(!workerResult && error.contains("main thread"), "worker-thread Start rejected");
    std::cout << "GUI dual entry, ABI, staging, ownership, rollback, services and lifecycle tests passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
