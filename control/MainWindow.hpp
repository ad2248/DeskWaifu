#pragma once

#include <QMainWindow>
#include <QJsonArray>

#include "DeskWaifuClient.hpp"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE
class HotkeyPanel;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

signals:
    void backendReady();

private:
    void bindControls();
    void refreshAll();
    void handleResponse(const QString& command, const QJsonObject& response);
    void logResponse(const QString& command, const QJsonObject& response);

    Ui::MainWindow* ui;
    HotkeyPanel* _hotkeys;
    DeskWaifuClient _client;
    bool _backendReadyEmitted = false;
    QString _hotkeyModelToken;
    QJsonObject _lastHotkeyResponse;
    bool _hotkeyQueryPending = false;
    void requestHotkeys();
};
