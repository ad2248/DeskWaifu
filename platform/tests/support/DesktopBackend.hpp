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
class DesktopBackend
{
public:
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
    QJsonObject State() { return Call("test.snapshot"); }
private:
    QTemporaryDir _installation;
    ProcessorEventLoop _loop;
    std::unique_ptr<PluginManager> _plugins;
    std::unique_ptr<EventEndpoint> _client;
};
