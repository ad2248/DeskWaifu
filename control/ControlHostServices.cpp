#include "ControlHostServices.hpp"
#include "DeskWaifuClient.hpp"
#include "MainWindow.hpp"

ControlHostServices::ControlHostServices(DeskWaifuClient& client, MainWindow& window)
    : _client(client), _window(window)
{
    connect(&client, &DeskWaifuClient::connectionChanged, this, &GuiHostServices::connectionChanged);
    connect(&client, &DeskWaifuClient::responseReceived, this, &GuiHostServices::responseReceived);
    connect(&client, &DeskWaifuClient::protocolError, this, &GuiHostServices::protocolError);
    connect(&client, &DeskWaifuClient::commandOutcomeUnknown, this, &GuiHostServices::commandOutcomeUnknown);
    connect(&client, &DeskWaifuClient::commandCancelled, this, &GuiHostServices::commandCancelled);
}

bool ControlHostServices::isConnected() const { return _client.isConnected(); }
bool ControlHostServices::isActive() const { return _window.isActiveWindow(); }
void ControlHostServices::sendCommand(const QString& command, const QJsonObject& parameters)
{
    _client.sendCommand(command, parameters);
}
void ControlHostServices::cancelQueuedCommand(const QString& command)
{
    _client.cancelQueuedCommand(command);
}
void ControlHostServices::setStatusMessage(const QString& message, int timeoutMs)
{
    _window.setStatusMessage(message, timeoutMs);
}
void ControlHostServices::connectToServer() { _client.connectToServer(); }
