#include "WaifulandClient.hpp"

#include <QDir>
#include <QJsonDocument>

WaifulandClient::WaifulandClient(QObject* parent)
    : QObject(parent)
{
    _reconnectTimer.setInterval(1500);
    _reconnectTimer.setSingleShot(true);

    connect(&_socket, &QLocalSocket::connected, this, &WaifulandClient::handleConnected);
    connect(&_socket, &QLocalSocket::disconnected, this, &WaifulandClient::handleDisconnected);
    connect(&_socket, &QLocalSocket::readyRead, this, &WaifulandClient::readResponses);
    connect(&_socket, &QLocalSocket::errorOccurred, this, &WaifulandClient::handleSocketError);
    connect(&_reconnectTimer, &QTimer::timeout, this, &WaifulandClient::connectToServer);
}

WaifulandClient::~WaifulandClient()
{
    disconnect(&_socket, nullptr, this, nullptr);
    _reconnectTimer.stop();
    _socket.abort();
}

void WaifulandClient::connectToServer()
{
    if (_socket.state() != QLocalSocket::UnconnectedState)
    {
        return;
    }
    _socket.connectToServer(socketPath(), QIODevice::ReadWrite);
}

void WaifulandClient::sendCommand(const QString& command, const QJsonObject& parameters)
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

bool WaifulandClient::isConnected() const
{
    return _socket.state() == QLocalSocket::ConnectedState;
}

QString WaifulandClient::socketPath() const
{
    const QByteArray runtimeDir = qgetenv("XDG_RUNTIME_DIR");
    return runtimeDir.isEmpty()
        ? QStringLiteral("/tmp/waifuland.sock")
        : QDir(QString::fromLocal8Bit(runtimeDir)).filePath(QStringLiteral("waifuland.sock"));
}

void WaifulandClient::readResponses()
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

void WaifulandClient::handleConnected()
{
    _reconnectTimer.stop();
    emit connectionChanged(true, tr("Connected to %1").arg(socketPath()));
    sendNext();
}

void WaifulandClient::handleDisconnected()
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
    emit connectionChanged(false, tr("Disconnected"));
    _reconnectTimer.start();
}

void WaifulandClient::handleSocketError(QLocalSocket::LocalSocketError error)
{
    Q_UNUSED(error)
    emit connectionChanged(false, _socket.errorString());
    if (_socket.state() == QLocalSocket::UnconnectedState && !_reconnectTimer.isActive())
    {
        _reconnectTimer.start();
    }
}

void WaifulandClient::sendNext()
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

bool WaifulandClient::isRetryableQuery(const QString& command)
{
    return command == QStringLiteral("get_status")
        || command == QStringLiteral("get_available_models")
        || command == QStringLiteral("get_current_model")
        || command == QStringLiteral("get_expressions")
        || command == QStringLiteral("get_motions")
        || command == QStringLiteral("get_model_position");
}
