#include "plugin/GuiPluginManager.hpp"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QPushButton>
#include <QScopeGuard>
#include <QTabWidget>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

class Host final : public GuiHostServices
{
public:
    bool connected = true;
    QStringList commands, cancellations;
    bool isConnected() const override { return connected; }
    void sendCommand(const QString& command, const QJsonObject&) override { commands.append(command); }
    void cancelQueuedCommand(const QString& command) override { cancellations.append(command); }
};

void CopyModule(const QString& source, const QString& root, const QString& name)
{
    Check(QDir().mkpath(root + '/' + name), "create plugin directory");
    Check(QFile::copy(source, root + '/' + name + '/' + name + ".so"), "copy module");
}
}

int main(int argc, char** argv)
try {
    QApplication app(argc, argv);
    Check(argc == 3, "usage: gui-shutdown-test system.so shutdown-pump.so");
    QTemporaryDir directory;
    Check(directory.isValid(), "private test directory");
    const auto marker = directory.path() + "/spawned";
    qputenv("DESKWAIFU_GUI_SHUTDOWN_MARKER", marker.toUtf8());

    // CMake places this executable in a dedicated directory without the real
    // backend. Even a regressed Stop can only launch this short-lived fixture.
    const auto backend = QCoreApplication::applicationDirPath() + "/deskwaifu";
    Check(!QFile::exists(backend), "fixture directory must not contain a backend");
    QFile script(backend);
    Check(script.open(QIODevice::WriteOnly | QIODevice::NewOnly), "create safe backend fixture");
    const auto removeFixture = qScopeGuard([&] { QFile::remove(backend); });
    script.write("#!/bin/sh\nprintf spawned >> \"$DESKWAIFU_GUI_SHUTDOWN_MARKER\"\n");
    script.close();
    Check(script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner), "executable fixture");

    for (bool disconnected : {true, false}) {
        const auto root = directory.path() + (disconnected ? "/disconnected" : "/queued");
        CopyModule(QString::fromLocal8Bit(argv[1]), root, "z-system");
        CopyModule(QString::fromLocal8Bit(argv[2]), root, "a-pump");
        Host host;
        host.setProperty("procDirectory", directory.path());
        QTabWidget tabs;
        GuiPluginManager manager(root, host, tabs);
        QString error;
        Check(manager.Start(&error), qPrintable(error));
        auto* restart = tabs.findChild<QPushButton*>(QStringLiteral("respawnButton"));
        Check(restart && restart->isEnabled(), "system page ready");
        restart->click();
        Check(host.commands.count("quit") == 1 && !restart->isEnabled(), "restart pending");
        if (disconnected) {
            host.connected = false;
            emit host.connectionChanged(false, QStringLiteral("fixture disconnected"));
        }
        // Reverse stop order: system Stop, then pump Stop with a live event loop.
        manager.Stop();
        Check(tabs.count() == 0, "pages destroyed after Stop");
        Check(!QFile::exists(marker), "stopped system plugin must not launch a backend during another plugin's Stop");
        Check(host.cancellations == QStringList{"quit"}, "teardown retracts queued quit even after service invalidation");
    }
    std::cout << "System plugin stops restart timers and cancels queued quit before teardown events\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
