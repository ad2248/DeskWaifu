// 定义 GUI 插件访问宿主命令、连接和传输代理的服务边界。
#pragma once

#include <QJsonObject>
#include <QObject>

class IGuiTransport;

// 应用实现的领域无关桥接接口，所有调用和信号均在 GUI 线程执行。
// 插件获得上下文作用域的门面，而非进程拥有的原始服务对象。
// 连接回调必须以上下文、已登记页面或其后代为接收者，不得使用无接收者的仿函数。
// Stop 前会移除这些接收者上的排队调用；任意外部接收者不受插件生命周期保护。
class GuiHostServices : public QObject
{
    Q_OBJECT
public:
    explicit GuiHostServices(QObject* parent = nullptr) : QObject(parent) {}
    // 返回当前是否已连接；失效门面返回 false。
    virtual bool isConnected() const = 0;
    // 提交命令，由响应或错误信号报告结果；初始化阶段或门面失效后不提交新命令。
    virtual void sendCommand(const QString& command, const QJsonObject& parameters = {}) = 0;
    // 仅用于清理，可在上下文失效后的 Stop 阶段调用。
    // 取消尚未发送的命令，无法撤回已发送的变更。
    virtual void cancelQueuedCommand(const QString& command) = 0;
    // 请求显示状态消息；timeoutMs 为 0 表示不指定自动清除期限，默认实现无操作。
    virtual void setStatusMessage(const QString& message, int timeoutMs = 0) { Q_UNUSED(message); Q_UNUSED(timeoutMs); }
    // 请求连接后端，结果由连接信号报告；默认实现无操作。
    virtual void connectToServer() {}
    // 一个上下文可登记一个插件拥有的 GUI 线程传输，也可在 Initialize 期间登记。
    // owner 必须是上下文本身；成功返回 true，拒绝登记返回 false。
    // 上下文失效时会在 Stop 前解除关联；门面和代理均不接管所有权。
    virtual bool registerTransport(IGuiTransport* transport, QObject* owner)
    { Q_UNUSED(transport); Q_UNUSED(owner); return false; }
    // 解除此前登记的传输关联，不销毁传输；不匹配的指针无操作。
    virtual void unregisterTransport(IGuiTransport* transport) { Q_UNUSED(transport); }
    // 报告后端已就绪；上下文门面仅在激活且有效时转发。
    virtual void notifyBackendReady() { emit backendReady(); }
    // 返回宿主报告的活动状态，失效门面返回 false；不等同于连接状态。
    virtual bool isActive() const { return true; }

signals:
    void connectionChanged(bool connected, const QString& message);
    void responseReceived(const QString& command, const QJsonObject& response);
    void protocolError(const QString& message);
    // 已发送的变更因断线无法确认结果，不得自动重试。
    void commandOutcomeUnknown(const QString& command);
    // 排队变更在发送前被取消。
    void commandCancelled(const QString& command);
    void backendReady();
    void activationChanged(bool active);
};
