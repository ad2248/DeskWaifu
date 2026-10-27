#pragma once

#include <QJsonObject>
#include <QObject>

class IGuiTransport;

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
    // Cleanup only: may be called during Stop after context invalidation.
    // Cancels unsent commands; it cannot retract an already sent mutation.
    virtual void cancelQueuedCommand(const QString& command) = 0;
    virtual void setStatusMessage(const QString& message, int timeoutMs = 0) { Q_UNUSED(message); Q_UNUSED(timeoutMs); }
    virtual void connectToServer() {}
    // A context may register one plugin-owned GUI-thread transport, including
    // during Initialize. Pass the context itself as owner. Context invalidation
    // detaches it before Stop; neither the facade nor broker takes ownership.
    virtual bool registerTransport(IGuiTransport* transport, QObject* owner)
    { Q_UNUSED(transport); Q_UNUSED(owner); return false; }
    virtual void unregisterTransport(IGuiTransport* transport) { Q_UNUSED(transport); }
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
