#pragma once

#include <QJsonObject>
#include <QObject>

// Domain-independent bridge, implemented by the application. All calls and
// signals run on the GUI thread. Plugins receive a context-scoped facade, not
// this process-owned object. Connect callbacks with the context, a registered
// page or one of its descendants as receiver, never an unscoped functor. These
// receivers have pending queued calls removed before Stop; arbitrary external
// receivers are not covered by the plugin lifecycle.
class GuiHostServices : public QObject
{
    Q_OBJECT
public:
    explicit GuiHostServices(QObject* parent = nullptr) : QObject(parent) {}
    virtual bool isConnected() const = 0;
    virtual void sendCommand(const QString& command, const QJsonObject& parameters = {}) = 0;
    virtual void cancelQueuedCommand(const QString& command) = 0;
    virtual void setStatusMessage(const QString& message, int timeoutMs = 0) { Q_UNUSED(message); Q_UNUSED(timeoutMs); }
    virtual void connectToServer() {}
    virtual void publishLog(const QString& message) { emit logMessage(message); }
    virtual void notifyBackendReady() { emit backendReady(); }
    virtual bool isActive() const { return true; }

signals:
    void connectionChanged(bool connected, const QString& message);
    void responseReceived(const QString& command, const QJsonObject& response);
    void protocolError(const QString& message);
    // A sent mutation lost its connection and must not be retried.
    void commandOutcomeUnknown(const QString& command);
    // A queued mutation was cancelled before it was sent.
    void commandCancelled(const QString& command);
    void logMessage(const QString& message);
    void backendReady();
    void activationChanged(bool active);
};
