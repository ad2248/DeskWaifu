#pragma once
#include <QScrollArea>
#include <QJsonObject>
#include <QTimer>
#include <QElapsedTimer>

class GuiHostServices;
namespace Ui { class SystemTab; }

class SystemTab : public QScrollArea
{
public:
    explicit SystemTab(GuiHostServices& services);
    ~SystemTab() override;
    void start();
private:
    void refreshAll();
    void updateSpawnButton();
    void startRespawn();
    void finishRespawn(const QString& message);
    void handleResponse(const QString& command, const QJsonObject& response);
    void requestRenderDevice();
    void showRenderDevice(const QJsonObject& response);
    void setStatusMessage(const QString& message, int timeout = 0);
    void logResponse(const QString& command, const QJsonObject& response);
    Ui::SystemTab* ui;
    GuiHostServices& _client;
    QString _procDirectory;
    QTimer _respawnTimer;
    QElapsedTimer _respawnElapsed;
    bool _respawnDisconnected = false;
    bool _renderDeviceQueryPending = false;
    bool _renderDeviceSetPending = false;
    QJsonObject _lastRenderDeviceResponse;
};
