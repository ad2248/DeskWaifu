#pragma once
#include "plugin/GuiHostServices.hpp"
#include <memory>

// Real dynamically loaded transport, exposed solely through the SDK bridge.
// 通过真实 IPC GUI 插件提供测试宿主服务，拥有私有安装及插件管理器。
// 仅在 GUI 主线程使用；注册传输由插件拥有，本对象不接管其所有权。
class LoadedGuiTransport final : public GuiHostServices
{
public:
    // 安装并启动真实 IPC 插件；失败或未注册传输时抛出 runtime_error。
    LoadedGuiTransport();
    // 在宿主桥仍有效时停止插件并释放测试安装。
    ~LoadedGuiTransport() override;
    // 返回传输连接状态，未注册传输时返回 false。
    bool isConnected() const override;
    // 转发连接请求；未注册传输时不做操作。
    void connectToServer() override;
    // 转发命令；未注册传输时不做操作。
    void sendCommand(const QString& command, const QJsonObject& parameters = {}) override;
    // 转发尚未发送命令的取消请求；未注册传输时不做操作。
    void cancelQueuedCommand(const QString& command) override;
    // 接受唯一非空传输并转发其信号，成功返回 true；测试封装不使用 owner。
    bool registerTransport(IGuiTransport* transport, QObject* owner) override;
    // 仅解除匹配传输的信号连接及弱引用，不销毁传输。
    void unregisterTransport(IGuiTransport* transport) override;
    // 返回当前 XDG_RUNTIME_DIR 下的 deskwaifu.sock 路径，不检查其存在性。
    QString socketPath() const;
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
