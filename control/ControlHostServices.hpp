#pragma once

#include "plugin/GuiHostServices.hpp"

class DeskWaifuClient;
class MainWindow;

// The only bridge from the control shell to the plugin SDK. No domain commands
// or page controls belong here.
class ControlHostServices final : public GuiHostServices
{
public:
    ControlHostServices(DeskWaifuClient& client, MainWindow& window);
    bool isConnected() const override;
    bool isActive() const override;
    void sendCommand(const QString& command, const QJsonObject& parameters = {}) override;
    void cancelQueuedCommand(const QString& command) override;
    void setStatusMessage(const QString& message, int timeoutMs = 0) override;
    void connectToServer() override;

private:
    DeskWaifuClient& _client;
    MainWindow& _window;
};
