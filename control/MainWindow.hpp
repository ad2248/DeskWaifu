#pragma once

#include <QMainWindow>
#include <QTimer>
#include <memory>

#include "DeskWaifuClient.hpp"

class GuiPluginManager;
class ControlHostServices;
class QLabel;
namespace Ui { class MainWindow; }

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr,
        const QString& procDirectory = QStringLiteral("/proc"));
    ~MainWindow() override;
    void setStatusMessage(const QString& message, int timeoutMs = 0);

signals:
    void backendReady();

private:
    std::unique_ptr<Ui::MainWindow> ui;
    DeskWaifuClient _client;
    std::unique_ptr<ControlHostServices> _services;
    std::unique_ptr<GuiPluginManager> _plugins;
    QLabel* _connectionStatusLabel;
    QLabel* _operationStatusLabel;
    QTimer _statusMessageTimer;
    bool _backendReadyEmitted = false;
};
