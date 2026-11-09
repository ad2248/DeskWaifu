#pragma once

#include "plugin/GuiHostServices.hpp"
#include "plugin/GuiTransport.hpp"
#include <QPointer>
#include <vector>

class MainWindow;

// The only bridge from the control shell to the plugin SDK. No domain commands
// or page controls belong here.
class ControlHostServices final : public GuiHostServices
{
public:
    explicit ControlHostServices(MainWindow& window);
    bool isConnected() const override;
    bool isActive() const override;
    void sendCommand(const QString& command, const QJsonObject& parameters = {}) override;
    void cancelQueuedCommand(const QString& command) override;
    void setStatusMessage(const QString& message, int timeoutMs = 0) override;
    void connectToServer() override;
    bool registerTransport(IGuiTransport* transport, QObject* owner) override;
    void unregisterTransport(IGuiTransport* transport) override;

private:
    void clearTransport();
    QPointer<IGuiTransport> _transport;
    QPointer<QObject> _transportOwner;
    std::vector<QMetaObject::Connection> _transportConnections;
    MainWindow& _window;
};
