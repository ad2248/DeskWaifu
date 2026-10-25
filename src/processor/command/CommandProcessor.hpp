#pragma once

#include "processor/Processor.hpp"
#include "contracts/ConfigStore.hpp"
#include "contracts/JsonHelpers.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <deque>
#include <map>

// 只拥有协议请求。每个领域一次一个请求，其他领域可独立前进。
// 领域请求/回复没有回调或关联编号，依靠有序队列与回复事件类型配对。
class CommandProcessor final : public Processor
{
public:
    explicit CommandProcessor(ProcessorEventLoop& loop) : Processor(loop),
        _endpoint(loop.CreateEndpoint("command-gateway", [](const EventDelivery&) {})) {}
    ~CommandProcessor() override { Stop(); }
    void Stop()
    {
        _stopped = true;
        if (_endpoint) _endpoint->Invalidate();
        auto dynamic = std::move(_dynamicPending);
        for (const auto& request : dynamic)
            Reply(request.second, R"({"ok":false,"error":"command processor stopped"})");
        auto pending = std::move(_pending);
        for (const auto& queue : pending) for (const auto& request : queue.second)
            Reply(request, R"({"ok":false,"error":"command processor stopped"})");
    }
    bool HandleEvent(const ProcessorEvent& event) override
    {
        if (event.type == "command.request") {
            Enqueue({event.data.value("json").toString().toStdString(),
                event.data.value("connection").toString().toStdString(), event.sender});
            return true;
        }
        QString domain;
        if (event.type == "model.result") domain = "interaction.model";
        else if (event.type == "interaction.result") domain = "interaction.command";
        else if (event.type == "ocr.result") domain = "ocr.command";
        else return false;
        if (event.receiver != this) return false;
        auto& queue = _pending[domain];
        if (!queue.empty()) {
            auto request = std::move(queue.front()); queue.pop_front();
            const auto reply = event.type == "model.result"
                ? QJsonDocument(event.data.value("reply").toJsonObject()).toJson(QJsonDocument::Compact).toStdString()
                : event.data.value("json").toString().toStdString();
            Reply(request, reply);
            if (!_stopped && !_pending[domain].empty()) Dispatch(domain);
        }
        return true;
    }
private:
    struct Request { std::string json, connection; IProcessor* requester; };
    void Reply(const Request& request, const std::string& json)
    {
        const bool quit = QJsonDocument::fromJson(QByteArray::fromStdString(request.json)).object().value("command") == "quit" &&
            QJsonDocument::fromJson(QByteArray::fromStdString(json)).object().value("ok").toBool();
        // connection 仅在 IPC/协议边界保留既有控制来源；不进入模型/OCR/朗读流程。
        Send("ipc.command.result", request.requester, {{"json", QString::fromStdString(json)},
            {"connection", QString::fromStdString(request.connection)}, {"quit", quit}});
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
            [this, request, command](const RequestResult& result) {
                _dynamicPending.erase(result.requestId);
                if (result.status == RequestStatus::NoProvider) { EnqueueLegacy(request, command); return; }
                const auto response = result.payload ? result.payload->data.value("json") : QVariant{};
                ConfigStore::Snapshot parsed; std::string error;
                if (result.status == RequestStatus::Replied && response.metaType().id() == QMetaType::QString &&
                    ConfigStore::Parse(response.toString().toStdString(), parsed, error, false))
                    Reply(request, QJsonDocument::fromJson(response.toString().toUtf8()).toJson(QJsonDocument::Compact).toStdString());
                else Reply(request, QJsonDocument(QJsonObject{{"ok", false}, {"error", result.status == RequestStatus::Replied
                    ? QStringLiteral("invalid command provider response")
                    : QString::fromLatin1(RequestStatusName(result.status))}}).toJson(QJsonDocument::Compact).toStdString());
            }) : 0;
        if (id) _dynamicPending.emplace(id, std::move(request));
        else Reply(request, R"({"ok":false,"error":"command gateway unavailable"})");
    }
    void EnqueueLegacy(Request request, const std::string& command)
    {
        // Only unmigrated domains use this transitional static routing.
        QString domain = "interaction.command";
        if (command == "set_model" || command == "next_model" || command == "prev_model" || command == "get_hotkeys" ||
            command == "trigger_hotkey" || command == "set_expression" || command == "do_motion" || command == "switch_skin")
            domain = "interaction.model";
        else if (command == "get_ocr_status" || command == "ocr_recognize" || command == "get_ocr_result" || command == "cancel_ocr")
            domain = "ocr.command";
        auto& queue = _pending[domain]; queue.push_back(std::move(request));
        if (queue.size() == 1) Dispatch(domain);
    }
    void Dispatch(const QString& domain)
    {
        const auto& request = _pending[domain].front();
        QVariantMap data{{"json", QString::fromStdString(request.json)}};
        if (domain == "interaction.command") data.insert("connection", QString::fromStdString(request.connection));
        Send(domain, nullptr, std::move(data));
    }
    std::map<QString, std::deque<Request>> _pending;
    std::unique_ptr<EventEndpoint> _endpoint;
    std::map<RequestId, Request> _dynamicPending;
    bool _stopped = false;
};
