#include "MainWindow.hpp"
#include "plugin/GuiTransport.hpp"
#include <QApplication>
#include <QLabel>
#include <QStringList>
#include <iostream>
#include <memory>

#define CHECK(c) do { if (!(c)) { std::cerr << __LINE__ << ": " << #c << '\n'; return 1; } } while (false)

class TestTransport final : public IGuiTransport
{
public:
    bool isConnected() const override { return connected; }
    void connectToServer() override
    {
        connected = true;
        emit connectionChanged(true, "test transport connected");
    }
    void sendCommand(const QString& command, const QJsonObject& parameters) override
    { lastCommand = command; lastParameters = parameters; }
    void cancelQueuedCommand(const QString& command) override { cancelled = command; }
    bool connected = false;
    QString lastCommand, cancelled;
    QJsonObject lastParameters;
};

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    qputenv("DESKWAIFU_CONTROL_PLUGIN_DIR", "");
    MainWindow window;
    auto& services = window.hostServices();
    CHECK(!services.isConnected());
    auto* label = window.findChild<QLabel*>("connectionStatusLabel");
    CHECK(label && label->text().contains(QStringLiteral("传输服务不可用")));
    QStringList logs;
    int responses = 0;
    QObject::connect(&services, &GuiHostServices::logMessage, &window,
        [&](const QString& message) { logs.append(message); });
    QObject::connect(&services, &GuiHostServices::responseReceived, &window,
        [&](const QString&, const QJsonObject&) { ++responses; });

    TestTransport transport, other;
    auto owner = std::make_unique<QObject>();
    CHECK(services.registerTransport(&transport, owner.get()));
    CHECK(!services.registerTransport(&other, owner.get()));
    services.connectToServer();
    CHECK(services.isConnected() && label->text() == "test transport connected");
    services.sendCommand("set_credentials", {{"password", "super-secret"}});
    CHECK(transport.lastCommand == "set_credentials");
    CHECK(transport.lastParameters.value("password").toString() == "super-secret");
    services.cancelQueuedCommand("set_credentials");
    CHECK(transport.cancelled == "set_credentials");
    emit transport.responseReceived("set_credentials", {{"ok", true}});
    CHECK(responses == 1);
    emit transport.protocolError("invalid transport response");
    emit transport.commandOutcomeUnknown("set_credentials");
    emit transport.commandCancelled("set_credentials");
    CHECK(logs.size() == 3 && !logs.join('\n').contains("super-secret"));

    services.unregisterTransport(&other);
    CHECK(services.isConnected());
    owner.reset();
    CHECK(!services.isConnected());
    emit transport.responseReceived("late", {{"ok", true}});
    CHECK(responses == 1);
    services.sendCommand("missing", {{"password", "super-secret"}});
    CHECK(responses == 2 && logs.size() == 4 && !logs.join('\n').contains("super-secret"));

    QObject nextOwner;
    auto next = std::make_unique<TestTransport>();
    CHECK(services.registerTransport(next.get(), &nextOwner));
    services.connectToServer();
    CHECK(services.isConnected());
    next.reset();
    CHECK(!services.isConnected());
    CHECK(services.registerTransport(&other, &nextOwner));
    services.unregisterTransport(&other);
    CHECK(!services.isConnected());
    return 0;
}
