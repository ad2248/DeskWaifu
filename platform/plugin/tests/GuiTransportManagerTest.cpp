#include "plugin/GuiPluginManager.hpp"
#include "plugin/GuiTransport.hpp"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTabWidget>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
QByteArray Read(const QString& path) { QFile file(path); Check(file.open(QIODevice::ReadOnly), "read lifecycle log"); return file.readAll(); }
class Host : public GuiHostServices
{
public:
    bool isConnected() const override { return false; }
    void sendCommand(const QString&, const QJsonObject&) override {}
    void cancelQueuedCommand(const QString&) override {}
    bool registerTransport(IGuiTransport* value, QObject* owner) override
    {
        ++registerCalls;
        if (transport || !value || !owner) return false;
        transport = value;
        transportDestroyed = connect(value, &QObject::destroyed, this, [this] {
            ++destroyedRegistrations;
            transport.clear();
        });
        return true;
    }
    void unregisterTransport(IGuiTransport* value) override
    {
        ++unregisterCalls;
        Check(value && transport == value, "only the registered live transport may be unregistered");
        disconnect(transportDestroyed);
        transport.clear();
        emit responseReceived("during-unregister", {});
        // A broker is allowed to pump events while detaching. Context, page and
        // descendant callbacks must already be fenced before this external call.
        QCoreApplication::processEvents();
    }
    QPointer<IGuiTransport> transport;
    QMetaObject::Connection transportDestroyed;
    int registerCalls = 0, unregisterCalls = 0, destroyedRegistrations = 0;
};
}

int main(int argc, char** argv)
try {
    QApplication app(argc, argv);
    Check(argc == 6, "expected transport fixture paths");
    QTemporaryDir directory;
    Check(directory.isValid(), "temporary transport fixtures");
    for (int i = 1; i < argc; ++i) {
        const auto root = directory.filePath(QString::number(i));
        Check(QDir().mkpath(root + "/transport"), "create transport plugin directory");
        Check(QFile::copy(argv[i], root + "/transport/transport.so"), "copy transport fixture");
        const auto log = root + "/lifecycle.log";
        QFile file(log); Check(file.open(QIODevice::WriteOnly), "create lifecycle log"); file.close();
        qputenv("DESKWAIFU_GUI_TRANSPORT_TEST_LOG", log.toUtf8());
        Host host;
        QTabWidget tabs;
        GuiPluginManager manager(root, host, tabs);
        QString error;
        const bool failed = i == 2 || i == 3;
        const bool released = i == 4, destroyed = i == 5;
        Check(manager.Start(&error) != failed, qPrintable(error));
        Check(host.registerCalls == (released ? 2 : 1), "invalid or duplicate registration never reaches broker");
        if (!failed) emit host.responseReceived("queued-before-stop", {});
        manager.Stop();
        manager.Stop();
        QCoreApplication::processEvents();
        const auto events = Read(log);
        Check(!host.transport && tabs.count() == 0, "transport and pages released on Stop or initialization rollback");
        Check(host.unregisterCalls == (destroyed ? 0 : released ? 2 : 1), "registration cleanup is idempotent and exact");
        Check(host.destroyedRegistrations == (destroyed ? 1 : 0), "transport is detached before plugin destroys it");
        Check(events.count("plugin:stop") == 1 && events.count("plugin:destroy") == 1, "rollback and Stop destroy each plugin once");
        Check(!events.contains("bad-"), "no registrations or direct/queued callbacks after context invalidation");
        if (!failed) {
            Check(manager.Start(&error), "restart can register a new transport");
            manager.Stop();
            Check(!host.transport, "restart cleanup releases replacement transport");
        }
    }
    std::cout << "GUI transport registration, rollback, ownership and invalidation tests passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
