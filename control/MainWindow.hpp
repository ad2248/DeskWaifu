#pragma once

#include <QMainWindow>
#include <QJsonArray>
#include <QTimer>

#include "DeskWaifuClient.hpp"
#include "DeskWaifuRespawner.hpp"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE
class HotkeyPanel;
class QLabel;
class QLineEdit;
class QPushButton;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr, const QString& procDirectory = QStringLiteral("/proc"));
    ~MainWindow() override;

    // 更新操作反馈，不替换连接状态。
    // 空消息立即清除反馈；正数超时表示延迟相应时间后清除。
    void setStatusMessage(const QString& message, int timeoutMs = 0);

signals:
    void backendReady();

private:
    void bindControls();
    void setupSpeechSettings();
    QLineEdit* _speechKey;
    QLabel* _speechStatus;
    QPushButton* _speechSave;
    QPushButton* _speechClear;
    bool _speechSetPending = false;
    void updateSpawnButton();
    void refreshAll();
    void handleResponse(const QString& command, const QJsonObject& response);
    void logResponse(const QString& command, const QJsonObject& response);

    Ui::MainWindow* ui;
    HotkeyPanel* _hotkeys;
    DeskWaifuClient _client;
    QString _procDirectory;
    DeskWaifuRespawner _respawner;
    QLabel* _connectionStatusLabel;
    QLabel* _operationStatusLabel;
    QTimer _statusMessageTimer;
    bool _backendReadyEmitted = false;
    QString _hotkeyModelToken;
    QJsonObject _lastHotkeyResponse;
    bool _hotkeyQueryPending = false;
    void requestHotkeys();
    void requestTransform();
    void requestHoverOpacity();
    void setHoverOpacity(const QJsonObject& parameters);
    void showHoverOpacity(const QJsonObject& response);
    bool _hoverQueryPending = false;
    bool _hoverSetPending = false;
    bool _hoverSupported = false;
    QJsonObject _lastHoverResponse;
    void requestMouseLook();
    void requestRenderDevice();
    void showRenderDevice(const QJsonObject& response);
    bool _renderDeviceQueryPending = false;
    bool _renderDeviceSetPending = false;
    QJsonObject _lastRenderDeviceResponse;
    void setMouseLook(double radius);
    void showMouseLook(double radius);
    bool _mouseLookQueryPending = false;
    bool _mouseLookSetPending = false;
    bool _mouseLookSupported = false;
    double _lastMouseLookRadius = 0.5;
    void queueTransformPreview();
    QJsonObject transformParameters() const;
    void showTransform(const QJsonObject& response);
    bool _transformQueryPending = false;
    bool _transformUpdating = false;
    bool _transformEditStarting = false;
    bool _transformOwned = false;
    bool _transformConflict = false;
    bool _transformPreviewPending = false;
    bool _transformPreviewInFlight = false;
    bool _transformSupported = false;
    bool _outputSwitchPending = false;
    int _surfaceWidth = 1, _surfaceHeight = 1;
    QJsonObject _lastTransformResponse;
    QString _transformSaveRequest;
    void sendTransformSave(const QString& command, QJsonObject parameters = {});
};
