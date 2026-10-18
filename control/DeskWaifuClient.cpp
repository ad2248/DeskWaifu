#include "DeskWaifuClient.hpp"

#include <QDir>
#include <QJsonDocument>

DeskWaifuClient::DeskWaifuClient(QObject* parent)
    : QObject(parent)
{
    _reconnectTimer.setInterval(1500);
    _reconnectTimer.setSingleShot(true);

    connect(&_socket, &QLocalSocket::connected, this, &DeskWaifuClient::handleConnected);
    connect(&_socket, &QLocalSocket::disconnected, this, &DeskWaifuClient::handleDisconnected);
    connect(&_socket, &QLocalSocket::readyRead, this, &DeskWaifuClient::readResponses);
    connect(&_socket, &QLocalSocket::errorOccurred, this, &DeskWaifuClient::handleSocketError);
    connect(&_reconnectTimer, &QTimer::timeout, this, &DeskWaifuClient::connectToServer);
}

DeskWaifuClient::~DeskWaifuClient()
{
    disconnect(&_socket, nullptr, this, nullptr);
    _reconnectTimer.stop();
    _socket.abort();
}

void DeskWaifuClient::connectToServer()
{
    if (_socket.state() != QLocalSocket::UnconnectedState)
    {
        return;
    }
    _socket.connectToServer(socketPath(), QIODevice::ReadWrite);
}

void DeskWaifuClient::sendCommand(const QString& command, const QJsonObject& parameters)
{
    QJsonObject request = parameters;
    request.insert(QStringLiteral("command"), command);
    _requests.enqueue({command, QJsonDocument(request).toJson(QJsonDocument::Compact) + '\n'});

    if (!isConnected())
    {
        connectToServer();
        return;
    }
    sendNext();
}

void DeskWaifuClient::cancelQueuedCommand(const QString& command)
{
    for (qsizetype i = _requests.size(); i-- > 0;)
        if (_requests[i].command == command) _requests.removeAt(i);
}

bool DeskWaifuClient::isConnected() const
{
    return _socket.state() == QLocalSocket::ConnectedState;
}

QString DeskWaifuClient::socketPath() const
{
    const QByteArray runtimeDir = qgetenv("XDG_RUNTIME_DIR");
    return runtimeDir.isEmpty()
        ? QStringLiteral("/tmp/deskwaifu.sock")
        : QDir(QString::fromLocal8Bit(runtimeDir)).filePath(QStringLiteral("deskwaifu.sock"));
}

void DeskWaifuClient::readResponses()
{
    _readBuffer += _socket.readAll();
    while (true)
    {
        const qsizetype newline = _readBuffer.indexOf('\n');
        if (newline < 0)
        {
            return;
        }

        const QByteArray line = _readBuffer.left(newline);
        _readBuffer.remove(0, newline + 1);
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
        {
            emit protocolError(tr("Invalid JSON response: %1").arg(error.errorString()));
            _inFlight = {};
            _waitingForResponse = false;
            sendNext();
            continue;
        }

        const QString command = _inFlight.command;
        _inFlight = {};
        _waitingForResponse = false;
        emit responseReceived(command, document.object());
        sendNext();
    }
}

void DeskWaifuClient::handleConnected()
{
    _reconnectTimer.stop();
    emit connectionChanged(true, tr("Connected to %1").arg(socketPath()));
    sendNext();
}

void DeskWaifuClient::handleDisconnected()
{
    if (_waitingForResponse)
    {
        if (isRetryableQuery(_inFlight.command))
        {
            _requests.prepend(_inFlight);
        }
        else
        {
            emit commandOutcomeUnknown(_inFlight.command);
        }
    }
    _inFlight = {};
    _waitingForResponse = false;
    _readBuffer.clear();
    // A queued toggle is meaningful only for the current connection/model state.
    for (qsizetype i = _requests.size(); i-- > 0;)
        if (_requests[i].command == QStringLiteral("quit") ||
            _requests[i].command == QStringLiteral("trigger_hotkey") ||
            _requests[i].command == QStringLiteral("set_hover_opacity") ||
            _requests[i].command == QStringLiteral("set_mouse_look") ||
            _requests[i].command.endsWith(QStringLiteral("transform_edit")) ||
            _requests[i].command == QStringLiteral("reset_transform_settings") ||
            _requests[i].command == QStringLiteral("retry_transform_save") ||
            _requests[i].command == QStringLiteral("reload_transform_settings") ||
            _requests[i].command == QStringLiteral("relocate_crop")) _requests.removeAt(i);
    emit connectionChanged(false, tr("Disconnected"));
    _reconnectTimer.start();
}

void DeskWaifuClient::handleSocketError(QLocalSocket::LocalSocketError error)
{
    Q_UNUSED(error)
    emit connectionChanged(false, _socket.errorString());
    if (_socket.state() == QLocalSocket::UnconnectedState && !_reconnectTimer.isActive())
    {
        _reconnectTimer.start();
    }
}

void DeskWaifuClient::sendNext()
{
    if (!isConnected() || _waitingForResponse || _requests.isEmpty())
    {
        return;
    }
    _inFlight = _requests.dequeue();
    _socket.write(_inFlight.payload);
    _socket.flush();
    _waitingForResponse = true;
}

bool DeskWaifuClient::isRetryableQuery(const QString& command)
{
    return command == QStringLiteral("get_status")
        || command == QStringLiteral("get_available_models")
        || command == QStringLiteral("get_current_model")
        || command == QStringLiteral("get_expressions")
        || command == QStringLiteral("get_hotkeys")
        || command == QStringLiteral("get_motions")
        || command == QStringLiteral("get_model_position")
        || command == QStringLiteral("get_transform_settings")
        || command == QStringLiteral("get_hover_opacity")
        || command == QStringLiteral("get_mouse_look")
        || command == QStringLiteral("get_transform_save_result");
}
