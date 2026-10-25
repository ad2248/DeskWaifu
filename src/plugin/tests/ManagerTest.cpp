#include "plugin/PluginManager.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLibrary>
#include <QTemporaryDir>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Drain() { for (int i = 0; i < 100; ++i) QCoreApplication::processEvents(); }
void Copy(const QString& source, const QString& root, const QString& name)
{
    Check(QDir().mkpath(root + '/' + name), "create plugin directory");
    Check(QFile::copy(source, root + '/' + name + '/' + name + ".so"), "copy plugin fixture");
}
QByteArray Read(const QString& path) { QFile file(path); Check(file.open(QIODevice::ReadOnly), "read file"); return file.readAll(); }
void Clear(const QString& path) { QFile file(path); Check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "clear log"); }
}

int main(int argc, char** argv)
try {
    QCoreApplication app(argc, argv);
    Check(argc == 8, "expected fixture paths");
    ProcessorEventLoop loop;
    QTemporaryDir directory;
    Check(directory.isValid(), "temporary directory");
    const auto config = directory.path() + "/config";
    const auto log = directory.path() + "/lifecycle.log";
    qputenv("DESKWAIFU_PLUGIN_TEST_LOG", log.toUtf8()); Clear(log);
    const auto root = directory.path() + "/plugins";
    Copy(argv[1], root, "z-service");
    Copy(argv[2], root, "a-consumer");
    // None of these near misses is a plugin.
    Check(QFile::copy(argv[1], root + "/flat.so"), "flat fixture");
    QDir().mkpath(root + "/wrong");
    Check(QFile::copy(argv[1], root + "/wrong/libwrong.so"), "prefix mismatch fixture");
    Copy(argv[1], root + "/nested", "deep");
    const auto paths = PluginManager::Discover(root);
    Check(paths.size() == 2 && paths[0].endsWith("/a-consumer/a-consumer.so") && paths[1].endsWith("/z-service/z-service.so"), "exact nonrecursive directory discovery");
    PluginManager manager(loop, root, config);
    ProcessorEventPtr retainedPluginEvent;
    auto observer = loop.CreateEndpoint("observer", [&](const EventDelivery& message) { retainedPluginEvent = message.payload; });
    observer->Subscribe("fixture.started");
    QString error;
    Check(manager.Start(&error), qPrintable(error));
    Check(manager.LoadedPlugins() == QStringList{"a-consumer", "z-service"}, "loaded names");
    Check(!manager.Start(&error), "duplicate Start rejected without changing running plugins");
    Drain();
    const auto events = Read(log);
    Check(events.startsWith("initialize\ninitialize\nstart\nstart\n") && events.contains("replied\n"), "two-phase startup resolves provider initialized after consumer");
    QJsonObject object;
    Check(ReadJsonObject(config + "/plugin/a-consumer/settings.json", object, &error) && object["plugin"] == "a-consumer", "per-plugin configuration path");
    manager.Stop(); Drain();
    const auto stopped = Read(log);
    Check(stopped.endsWith("stop\nstop\ndestroy\ndestroy\n") && !stopped.contains("bad-stop-active"), "stop all before destroying, with inactive endpoints");
    Check(manager.LoadedPlugins().isEmpty(), "stop clears plugins");
    Check(retainedPluginEvent && retainedPluginEvent->type == "fixture-from-plugin", "plugin event can outlive its plugin instance");
    {
        QLibrary probe(paths[1]);
        Check(probe.load(), "probe retained library");
        probe.unload();
    }
    Check(!Read(log).contains("unload\n"), "library stays mapped after manager Stop and explicit unload attempt");
    retainedPluginEvent.reset();
    Check(Read(log).contains("payload-destroy\n"), "plugin-defined event safely destroyed after Stop");
    auto client = loop.CreateEndpoint("test-client", [](const EventDelivery&) {});
    RequestStatus status = RequestStatus::Replied;
    client->Request("fixture.compute", std::make_shared<ProcessorEvent>(), [&](const RequestResult& result) { status = result.status; });
    Drain(); Check(status == RequestStatus::NoProvider, "stop withdraws capabilities");
    Check(manager.Start(&error), "manager may restart, no leaked registrations");
    manager.Stop(); Drain();

    for (int i = 3; i < argc; ++i) {
        Clear(log);
        const auto failureRoot = directory.path() + "/failure-" + QString::number(i);
        Copy(argv[1], failureRoot, "a-good");
        Copy(argv[i], failureRoot, "z-bad");
        PluginManager failing(loop, failureRoot, config);
        Check(!failing.Start(&error) && !error.isEmpty(), "bad plugin rejected with reason");
        Check(failing.LoadedPlugins() == QStringList{"a-good"}, "bad plugin cannot disable healthy plugins");
        Drain();
        const auto failureEvents = Read(log);
        const bool initializedBad = i == 3 || i == 4;
        const auto lines = failureEvents.split('\n');
        Check(lines.count("stop") == (initializedBad ? 1 : 0) &&
              lines.count("destroy") == (initializedBad ? 1 : 0), "rollback stops/destroys only the failed instance");
        status = RequestStatus::Replied;
        client->Request("fixture.compute", std::make_shared<ProcessorEvent>(), [&](const RequestResult& result) { status = result.status; });
        Drain(); Check(status == RequestStatus::Replied, "healthy provider remains callable after another plugin failed");
        failing.Stop();
        client->Request("fixture.compute", std::make_shared<ProcessorEvent>(), [&](const RequestResult& result) { status = result.status; });
        Drain(); Check(status == RequestStatus::NoProvider, "Stop withdraws surviving providers");
    }
    PluginManager empty(loop, directory.path() + "/missing", config);
    Check(empty.Start(&error) && empty.LoadedPlugins().isEmpty(), "missing optional plugin root is empty");

    const auto json = directory.path() + "/atomic/config.json";
    Check(WriteJsonObject(json, {{"value", 1}}, &error), "write creates parent");
    Check(WriteJsonObject(json, {{"value", 2}}, &error), "replace config");
    Check(ReadJsonObject(json, object, &error) && object["value"] == 2, "read replacement");
    std::atomic<bool> done{false}, valid{true};
    std::thread reader([&] {
        while (!done) {
            QJsonObject read;
            if (!ReadJsonObject(json, read) || !read["value"].isDouble()) valid = false;
        }
    });
    for (int i = 0; i < 100; ++i) Check(WriteJsonObject(json, {{"value", i}, {"padding", QString(4096, 'x')}}, &error), "atomic write iteration");
    done = true; reader.join();
    Check(valid, "concurrent readers see complete JSON, never partial writes");
    QFile bad(directory.path() + "/bad.json");
    Check(bad.open(QIODevice::WriteOnly), "bad JSON fixture"); bad.write("{invalid"); bad.close();
    const auto previous = object;
    Check(!ReadJsonObject(bad.fileName(), object, &error) && object == previous && !error.isEmpty(), "parse failure preserves output");
    QFile array(directory.path() + "/array.json");
    Check(array.open(QIODevice::WriteOnly), "array JSON fixture"); array.write("[]"); array.close();
    Check(!ReadJsonObject(array.fileName(), object, &error), "non-object JSON rejected");
    const auto before = Read(json);
    Check(!WriteJsonObject(json + "/impossible.json", {}, &error) && Read(json) == before, "failed write cannot truncate existing destination ancestor");
    Check(!WriteJsonObject(directory.path(), {}, &error), "directory destination rejected");
    std::cout << "Plugin discovery, ABI, lifecycle, rollback and atomic JSON tests passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
