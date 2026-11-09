#pragma once
#include "plugin/PluginManager.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <stdexcept>

// Integration fixture speaks only the test module's JSON protocol. No plugin
// implementation headers or private static libraries cross into the test.
// 在私有临时安装中加载桌面设置测试模块，拥有事件循环、插件管理器及请求端点。
// 构造、调用及销毁均应在事件循环所属线程执行；等待响应时会泵送 Qt 事件。
class DesktopBackend
{
public:
    // 安装并启动模块，以 configPath 初始化设置；任一步失败均抛出 runtime_error。
    explicit DesktopBackend(const QString& configPath)
    {
        if (!_installation.isValid() || !QDir().mkpath(_installation.path() + "/settings") ||
            !QFile::copy(QStringLiteral(DESKTOP_SETTINGS_FIXTURE), _installation.path() + "/settings/settings.so"))
            throw std::runtime_error("install desktop settings fixture");
        _plugins.reset(new PluginManager(_loop, _installation.path(), _installation.path()));
        QString error;
        if (!_plugins->Start(&error)) throw std::runtime_error(error.toStdString());
        _client = _loop.CreateEndpoint("gui-test-backend", [](const EventDelivery&) {});
        if (!Call("test.initialize", {{"path", configPath}}).value("ok").toBool())
            throw std::runtime_error("initialize desktop settings fixture");
    }
    // 以 owner 标识连接发送测试命令，覆盖 input 中的 command 并返回 JSON 响应。
    // 最多等待约三秒；未完成或响应为空时抛出 runtime_error。
    QJsonObject Call(const QString& command, QJsonObject input = {}, const QString& owner = QStringLiteral("gui"))
    {
        input.insert("command", command);
        bool done = false; QJsonObject response;
        const auto id = _client->Request("test.desktop.settings/v1",
            std::make_shared<ProcessorEvent>("test.settings", nullptr, nullptr,
                QVariantMap{{"input", input}, {"connection", owner}}),
            [&](const RequestResult& result) {
                if (result.status == RequestStatus::Replied && result.payload)
                    response = result.payload->data.value("response").toJsonObject();
                done = true;
            });
        QElapsedTimer timer; timer.start();
        while (id && !done && timer.elapsed() < 3000) { QCoreApplication::processEvents(); QThread::msleep(1); }
        if (!done || response.isEmpty()) throw std::runtime_error("desktop test protocol completion");
        return response;
    }
    // 返回测试模块的当前状态快照，失败语义与 Call 相同。
    QJsonObject State() { return Call("test.snapshot"); }
private:
    QTemporaryDir _installation;
    ProcessorEventLoop _loop;
    std::unique_ptr<PluginManager> _plugins;
    std::unique_ptr<EventEndpoint> _client;
};
