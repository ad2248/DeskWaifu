#pragma once

#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QQueue>
#include <QTimer>

class WaifulandClient : public QObject
{
    Q_OBJECT

public:
    explicit WaifulandClient(QObject* parent = nullptr);
    ~WaifulandClient() override;

    void connectToServer();
    void sendCommand(const QString& command, const QJsonObject& parameters = {});
    bool isConnected() const;
    QString socketPath() const;

signals:
    void connectionChanged(bool connected, const QString& message);
    void responseReceived(const QString& command, const QJsonObject& response);
    void commandOutcomeUnknown(const QString& command);
    void protocolError(const QString& message);

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
