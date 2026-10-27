#include "ControlHostServices.hpp"
#include "MainWindow.hpp"

ControlHostServices::ControlHostServices(MainWindow& window)
    : _window(window)
{
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

bool ControlHostServices::isConnected() const { return _transport && _transport->isConnected(); }
bool ControlHostServices::isActive() const { return _window.isActiveWindow(); }
void ControlHostServices::sendCommand(const QString& command, const QJsonObject& parameters)
{
    if (_transport) _transport->sendCommand(command, parameters);
    else {
        const auto error = MainWindow::tr("命令传输服务不可用");
        emit protocolError(error);
        emit responseReceived(command, {{"ok", false}, {"error", error}});
    }
}
void ControlHostServices::cancelQueuedCommand(const QString& command)
{
    if (_transport) _transport->cancelQueuedCommand(command);
}
void ControlHostServices::setStatusMessage(const QString& message, int timeoutMs)
{
    _window.setStatusMessage(message, timeoutMs);
}
void ControlHostServices::connectToServer()
{
    if (_transport) _transport->connectToServer();
    else emit connectionChanged(false, MainWindow::tr("命令传输服务不可用"));
}

bool ControlHostServices::registerTransport(IGuiTransport* transport, QObject* owner)
{
    if (_transport || !transport || !owner || transport->thread() != thread() || owner->thread() != thread()) return false;
    _transport = transport;
    _transportOwner = owner;
    _transportConnections = {
        connect(transport, &GuiHostServices::connectionChanged, this, &GuiHostServices::connectionChanged),
        connect(transport, &GuiHostServices::responseReceived, this, &GuiHostServices::responseReceived),
        connect(transport, &GuiHostServices::protocolError, this, &GuiHostServices::protocolError),
        connect(transport, &GuiHostServices::commandOutcomeUnknown, this, &GuiHostServices::commandOutcomeUnknown),
        connect(transport, &GuiHostServices::commandCancelled, this, &GuiHostServices::commandCancelled),
        connect(transport, &QObject::destroyed, this, [this] { clearTransport(); }),
        connect(owner, &QObject::destroyed, this, [this] { clearTransport(); })
    };
    return true;
}

void ControlHostServices::unregisterTransport(IGuiTransport* transport)
{
    if (_transport == transport) clearTransport();
}

void ControlHostServices::clearTransport()
{
    for (const auto& connection : _transportConnections) disconnect(connection);
    _transportConnections.clear();
    _transport.clear();
    _transportOwner.clear();
    emit connectionChanged(false, MainWindow::tr("命令传输服务不可用"));
}
