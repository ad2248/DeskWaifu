#pragma once
#include <QScrollArea>
#include <QJsonObject>
#include <QTimer>
#include "DeskWaifuRespawner.hpp"

class GuiHostServices;
namespace Ui { class SystemTab; }

class SystemTab : public QScrollArea
{
public:
    explicit SystemTab(GuiHostServices& services);
    ~SystemTab() override;
    void start();
    void stop() noexcept;
private:
    void refreshAll();
    void updateSpawnButton();
    void handleResponse(const QString& command, const QJsonObject& response);
    void requestRenderDevice();
    void showRenderDevice(const QJsonObject& response);
    void setStatusMessage(const QString& message, int timeout = 0);
    void logResponse(const QString& command, const QJsonObject& response);
    Ui::SystemTab* ui;
    GuiHostServices& _client;
    QString _procDirectory;
    QTimer _processTimer;
    DeskWaifuRespawner _respawner;
    bool _renderDeviceQueryPending = false;
    bool _renderDeviceSetPending = false;
    QJsonObject _lastRenderDeviceResponse;
};
