#pragma once

#include "processor/Processor.hpp"
#include "plugin/PluginContext.hpp"
#include "contracts/ConfigStore.hpp"
#include "contracts/JsonHelpers.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <map>

// 只拥有协议请求。领域插件通过 command.<name>/v1 能力提供命令；
// 每个请求由 SDK requestId 关联，网关不枚举领域或调用其实现。
class CommandProcessor final : public Processor
{
public:
    // 生产端点由 context 持有并参与统一失效屏障；独立单测可使用本地端点。
    explicit CommandProcessor(ProcessorEventLoop& loop, PluginContext* context = nullptr) : Processor(loop),
        _ownedEndpoint(context ? nullptr : loop.CreateEndpoint("command-gateway", [](const EventDelivery&) {})),
        _endpoint(context ? context->CreateEndpoint("command-gateway", [](const EventDelivery&) {}) : _ownedEndpoint.get()) {}
    ~CommandProcessor() override { Stop(); }
    void Stop() noexcept override
    {
        if (_stopped) return;
        _stopped = true;
        if (_endpoint) _endpoint->Invalidate();
        auto pending = std::move(_pending);
        for (const auto& request : pending)
            Reply(request.second, R"({"ok":false,"error":"command processor stopped"})");
        Processor::Stop();
    }
    bool HandleEvent(const ProcessorEvent& event) override
    {
        if (event.type == "command.request") {
            Enqueue({event.data.value("json").toString().toStdString(),
                event.data.value("connection").toString().toStdString(), event.sender});
            return true;
        }
        if (event.type == "command.disconnected") Cancel(event.data.value("connection").toString().toStdString());
        // Disconnect is also consumed by desktop session/lipstream owners.
        return false;
    }
private:
    struct Request { std::string json, connection; IProcessorPlugin* requester; };
    void Reply(const Request& request, const std::string& json)
    {
        if (!request.requester) return;
        const bool quit = QJsonDocument::fromJson(QByteArray::fromStdString(request.json)).object().value("command") == "quit" &&
            QJsonDocument::fromJson(QByteArray::fromStdString(json)).object().value("ok").toBool();
        // connection 仅标识外部控制会话，不是进程内对象或回调。
        // 已完成的结果不依赖网关继续注册；receiver 的存活检查仍由事件循环保证。
        _loop.Post({"ipc.command.result", nullptr, request.requester, {{"json", QString::fromStdString(json)},
            {"connection", QString::fromStdString(request.connection)}, {"quit", quit}}});
    }
    void Enqueue(Request request)
    {
        if (_stopped) { Reply(request, R"({"ok":false,"error":"command processor stopped"})"); return; }
        ConfigStore::Snapshot parsed; std::string error, command;
        if (!ConfigStore::Parse(request.json, parsed, error, false)) {
            Reply(request, R"({"ok":false,"error":"Invalid JSON request or duplicate key"})"); return;
        }
        try {
            command = JsonGetString(parsed, "command");
            if (JsonGetBool(parsed, "stream_source") && request.connection.empty()) {
                Reply(request, R"({"ok":false,"error":"stream requires a connection"})"); return;
            }
        } catch (const std::invalid_argument&) {
            Reply(request, R"({"ok":false,"error":"invalid command fields"})"); return;
        }
        auto payload = std::make_shared<ProcessorEvent>("command.invoke", nullptr, nullptr,
            QVariantMap{{"json", QString::fromStdString(request.json)},
                        {"connection", QString::fromStdString(request.connection)}});
        const auto id = _endpoint ? _endpoint->Request("command." + QString::fromStdString(command) + "/v1", payload,
            [this](const RequestResult& result) {
                const auto found = _pending.find(result.requestId);
                if (found == _pending.end()) return;
                auto request = std::move(found->second);
                _pending.erase(found);
                const auto response = result.payload ? result.payload->data.value("json") : QVariant{};
                ConfigStore::Snapshot parsed; std::string error;
                if (result.status == RequestStatus::Replied && response.metaType().id() == QMetaType::QString &&
                    ConfigStore::Parse(response.toString().toStdString(), parsed, error, false))
                    Reply(request, QJsonDocument::fromJson(response.toString().toUtf8()).toJson(QJsonDocument::Compact).toStdString());
                else Reply(request, QJsonDocument(QJsonObject{{"ok", false}, {"error", result.status == RequestStatus::Replied
                    ? QStringLiteral("invalid command provider response")
                    : QString::fromLatin1(RequestStatusName(result.status))}}).toJson(QJsonDocument::Compact).toStdString());
            }) : 0;
        if (id) _pending.emplace(id, std::move(request));
        else Reply(request, R"({"ok":false,"error":"command gateway unavailable"})");
    }
    void Cancel(const std::string& connection)
    {
        for (auto it = _pending.begin(); it != _pending.end();) {
            if (it->second.connection != connection) { ++it; continue; }
            _endpoint->Cancel(it->first);
            it = _pending.erase(it);
        }
    }
    std::unique_ptr<EventEndpoint> _ownedEndpoint;
    EventEndpoint* _endpoint;
    std::map<RequestId, Request> _pending;
    bool _stopped = false;
};
