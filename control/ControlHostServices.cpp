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
    // Transport diagnostics must remain available without the optional log
    // viewer. Never log raw request/response bodies here (they may be secrets).
    connect(this, &GuiHostServices::protocolError, this, [this](const QString& message) {
        publishLog(message);
        setStatusMessage(message, 5000);
    });
    connect(this, &GuiHostServices::commandOutcomeUnknown, this, [this](const QString& command) {
        const auto message = MainWindow::tr("连接断开：命令“%1”的执行结果未知，未重试该命令。").arg(command);
        publishLog(message);
        setStatusMessage(message, 8000);
    });
    connect(this, &GuiHostServices::commandCancelled, this, [this](const QString& command) {
        const auto message = MainWindow::tr("连接断开：排队中的命令“%1”已在发送前取消。").arg(command);
        publishLog(message);
        setStatusMessage(message, 8000);
    });
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
