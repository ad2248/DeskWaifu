#pragma once
#include "plugin/GuiHostServices.hpp"
#include <memory>

// Real dynamically loaded transport, exposed solely through the SDK bridge.
class LoadedGuiTransport final : public GuiHostServices
{
public:
    LoadedGuiTransport();
    ~LoadedGuiTransport() override;
    bool isConnected() const override;
    void connectToServer() override;
    void sendCommand(const QString& command, const QJsonObject& parameters = {}) override;
    void cancelQueuedCommand(const QString& command) override;
    bool registerTransport(IGuiTransport* transport, QObject* owner) override;
    void unregisterTransport(IGuiTransport* transport) override;
    QString socketPath() const;
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
