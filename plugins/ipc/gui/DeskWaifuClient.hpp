#pragma once

#include <QJsonObject>
#include <QLocalSocket>
#include "plugin/GuiTransport.hpp"
#include <QQueue>
#include <QTimer>

class DeskWaifuClient : public IGuiTransport
{
    Q_OBJECT

public:
    explicit DeskWaifuClient(QObject* parent = nullptr);
    ~DeskWaifuClient() override;

    void connectToServer() override;
    void sendCommand(const QString& command, const QJsonObject& parameters = {}) override;
    void cancelQueuedCommand(const QString& command) override;
    bool isConnected() const override;
    QString socketPath() const;

private slots:
    void readResponses();
    void handleConnected();
    void handleDisconnected();
    void handleSocketError(QLocalSocket::LocalSocketError error);

private:
    struct Request
    {
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
