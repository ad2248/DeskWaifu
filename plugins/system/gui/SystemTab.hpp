#pragma once
#include <QScrollArea>
#include <QJsonObject>
#include <QTimer>
#include "DeskWaifuRespawner.hpp"

class GuiHostServices;
namespace Ui { class SystemTab; }

// 展示系统状态和渲染设备设置，协调后端的启动、关闭与重启。
// 在 GUI 线程使用；借用的 services 必须比页面存活更久。
class SystemTab : public QScrollArea
{
public:
    explicit SystemTab(GuiHostServices& services);
    ~SystemTab() override;
    // 已连接时异步查询后端状态和渲染设备设置。
    void start();
    // 停止进程轮询和重启流程，不关闭当前运行的后端。
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
