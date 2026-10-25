#include "DeskWaifuClient.hpp"
#include "processor/ipc/IpcSocketPath.hpp"

#include <QJsonDocument>
#include <QStringList>

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
    return QString::fromStdString(IpcSocketPath());
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
            emit protocolError(tr("命令“%1”的 JSON 响应无效：%2")
                .arg(_inFlight.command, error.error == QJsonParseError::NoError
                    ? tr("预期为 JSON 对象") : error.errorString()));
            // 关联关系已不再可信。断开后只重试安全查询，
            // 对已发送的修改操作报告结果未知。
            _socket.abort();
            return;
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
    emit connectionChanged(true, tr("已连接到 %1").arg(socketPath()));
    sendNext();
}

void DeskWaifuClient::handleDisconnected()
{
    QString unknownCommand;
    if (_waitingForResponse)
    {
        if (isRetryableQuery(_inFlight.command))
        {
            _requests.prepend(_inFlight);
        }
        else
        {
            unknownCommand = _inFlight.command;
        }
    }
    _inFlight = {};
    _waitingForResponse = false;
    _readBuffer.clear();
    QStringList cancelled;
    // 只有显式允许重试的查询才能跨连接重试。
    for (qsizetype i = 0; i < _requests.size();) {
        if (isRetryableQuery(_requests[i].command)) ++i;
        else cancelled.append(_requests.takeAt(i).command);
    }
    if (!unknownCommand.isEmpty()) emit commandOutcomeUnknown(unknownCommand);
    for (const auto& command : cancelled) emit commandCancelled(command);
    emit connectionChanged(false, tr("连接已断开"));
    _reconnectTimer.start();
}

void DeskWaifuClient::handleSocketError(QLocalSocket::LocalSocketError error)
{
    Q_UNUSED(error)
    emit connectionChanged(false, _socket.errorString());
    // Qt 可能在切换到 UnconnectedState 前发出错误信号。
    // 不论信号到达时处于什么状态，都要安排下一次尝试。
    if (!_reconnectTimer.isActive())
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
        || command == QStringLiteral("get_render_device")
        || command == QStringLiteral("get_transform_save_result");
}
