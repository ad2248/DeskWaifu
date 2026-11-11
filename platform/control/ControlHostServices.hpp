#pragma once

#include "plugin/GuiHostServices.hpp"
#include "plugin/GuiTransport.hpp"
#include <QPointer>
#include <vector>

class MainWindow;

// The only bridge from the control shell to the plugin SDK. No domain commands
// or page controls belong here.
// 在 GUI 线程转发宿主服务、状态提示及传输信号，不拥有注册的传输或 owner。
// window 必须比本对象存活更久；传输或 owner 销毁时自动解除注册。
class ControlHostServices final : public GuiHostServices {
public:
    explicit ControlHostServices(MainWindow& window);
    // 无传输时返回 false，否则返回传输的连接状态。
    bool isConnected() const override;
    // 返回控制窗口是否为当前活动窗口。
    bool isActive() const override;
    // 转发命令；没有传输时发出协议错误和失败响应，而非静默丢弃。
    void sendCommand(const QString& command, const QJsonObject& parameters = { }) override;
    // 仅转发排队命令取消请求；没有传输时不做操作。
    void cancelQueuedCommand(const QString& command) override;
    // 在窗口显示提示；正超时值用于自动清除，零表示持续显示。
    void setStatusMessage(const QString& message, int timeoutMs = 0) override;
    // 请求传输连接；没有传输时发出未连接状态。
    void connectToServer() override;
    // 仅接受同线程、非空的传输与 owner；已有传输时拒绝，成功返回 true。
    bool registerTransport(IGuiTransport* transport, QObject* owner) override;
    // 仅解除匹配的当前传输，断开信号并通知未连接；不销毁传输。
    void unregisterTransport(IGuiTransport* transport) override;

private:
    void clearTransport();
    QPointer<IGuiTransport> _transport;
    QPointer<QObject> _transportOwner;
    std::vector<QMetaObject::Connection> _transportConnections;
    MainWindow& _window;
};
