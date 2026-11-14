#pragma once

#include "plugin/GuiTransport.hpp"
#include <QJsonObject>
#include <QLocalSocket>
#include <QQueue>
#include <QTimer>

// 基于本地套接字串行发送 JSON 命令，并在断线后仅重试白名单查询。
// 在所属 Qt 线程调用；同一连接最多有一个等待回复的请求。
class DeskWaifuClient : public IGuiTransport {
    Q_OBJECT

public:
    explicit DeskWaifuClient(QObject* parent = nullptr);
    ~DeskWaifuClient() override;

    // 异步连接；已连接或正在连接时不重复发起。
    void connectToServer() override;
    // 将命令排入 FIFO 队列，未连接时同时尝试连接。
    void sendCommand(const QString& command, const QJsonObject& parameters = { }) override;
    // 移除尚未发送的同名命令，不取消已经发送的请求。
    void cancelQueuedCommand(const QString& command) override;
    // 仅在套接字处于 ConnectedState 时返回 true。
    bool isConnected() const override;
    // 返回当前用户及运行时目录对应的服务端套接字路径。
    QString socketPath() const;

private slots:
    void readResponses();
    void handleConnected();
    void handleDisconnected();
    void handleSocketError(QLocalSocket::LocalSocketError error);

private:
    struct Request {
        QString command;
        QByteArray payload;
    };

    void sendNext();
    static bool isRetryableQuery(const QString& command);

    QLocalSocket _socket;
    QQueue<Request> _requests;
    Request _inFlight;
    QByteArray _readBuffer;
    QTimer _reconnectTimer;
    bool _waitingForResponse = false;
};
