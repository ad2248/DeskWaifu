#pragma once

#include <QMainWindow>
#include <QTimer>
#include <memory>

#include "plugin/GuiHostServices.hpp"

class GuiPluginManager;
class ControlHostServices;
class QLabel;
namespace Ui {
class MainWindow;
}

// 控制面板外壳：拥有宿主服务与 GUI 插件管理器，业务页面由插件提供。
// 在 GUI 主线程构造和销毁；析构时先停止插件，确保服务及页面容器仍然有效。
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // 加载并启动 GUI 插件后请求连接；procDirectory 通过宿主属性提供给插件。
    explicit MainWindow(QWidget* parent = nullptr, const QString& procDirectory = QStringLiteral("/proc"));
    ~MainWindow() override;
    // 替换操作提示；空消息清除提示，非空且 timeoutMs 为正时定时清除。
    void setStatusMessage(const QString& message, int timeoutMs = 0);
    // 返回窗口拥有的宿主服务引用，有效期不超过窗口生命周期。
    GuiHostServices& hostServices() const;

signals:
    // 首次收到宿主服务的后端就绪通知时发出，每个窗口至多一次。
    void backendReady();

private:
    std::unique_ptr<Ui::MainWindow> ui;
    std::unique_ptr<ControlHostServices> _services;
    std::unique_ptr<GuiPluginManager> _plugins;
    QLabel* _connectionStatusLabel;
    QLabel* _operationStatusLabel;
    QTimer _statusMessageTimer;
    bool _backendReadyEmitted = false;
};
