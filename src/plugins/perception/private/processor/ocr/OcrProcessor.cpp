#include "OcrProcessor.hpp"
#include "processor/Events.hpp"
#include "plugin/PluginContext.hpp"
#include <QJsonArray>
#include <QJsonDocument>

OcrProcessor::OcrProcessor(ProcessorEventLoop& loop)
    : OcrProcessor(loop, std::unique_ptr<OcrClient>(new OcrClient(OcrClient::DefaultExecutable()))) {}

OcrProcessor::OcrProcessor(ProcessorEventLoop& loop, std::unique_ptr<OcrClient> client)
    : OcrProcessor(loop, std::move(client), nullptr) {}

OcrProcessor::OcrProcessor(PluginContext& context)
    : OcrProcessor(context.Loop(), std::unique_ptr<OcrClient>(new OcrClient(OcrClient::DefaultExecutable())), &context) {}

OcrProcessor::OcrProcessor(ProcessorEventLoop& loop, std::unique_ptr<OcrClient> client, PluginContext* context)
    : Processor(loop), _client(std::move(client))
{
    EventEndpoint::Handler handler = [this](const EventDelivery& delivery) {
        if (delivery.kind == EventDelivery::Kind::Request && delivery.payload)
            Recognize(*delivery.payload, delivery.requestId);
        else if (delivery.kind == EventDelivery::Kind::Cancellation && delivery.requestId == _request && _client)
            _client->Cancel(_transportRequest);
    };
    if (context) _endpoint = context->CreateEndpoint("ocr", std::move(handler));
    else {
        _ownedEndpoint = loop.CreateEndpoint("ocr", std::move(handler));
        _endpoint = _ownedEndpoint.get();
    }
    if (_endpoint) _endpoint->Provide("ocr.recognize/v1");
    connect(_client.get(), &OcrClient::WorkerStopped, this, [this] { _retiringImage.clear(); });
    connect(_client.get(), &OcrClient::Finished, this, [this](const QString& request, const QJsonObject& response) {
        if (_transportRequest.isEmpty() || request != _transportRequest) return;
        auto* receiver = _requester;
        const auto pending = _request;
        _requester = nullptr; _transportRequest.clear(); _request = 0;
        if (!response.value("ok").toBool() && _client->Status().value("running").toBool()) _retiringImage = _image;
        _image.clear();
        if (!response.value("ok").toBool()) Complete(ProcessorEvents::Failed, receiver, pending, {{"error", response.value("error").toString()}});
        else {
            QStringList texts;
            const auto result = response.value("result").toObject();
            for (const auto& line : result.value("lines").toArray()) texts.append(line.toObject().value("text").toString());
            Complete(ProcessorEvents::Recognized, receiver, pending, {{"text", texts.join('\n')}, {"result", result.toVariantMap()}});
        }
    });
}

OcrProcessor::~OcrProcessor()
{ Stop(); }

void OcrProcessor::Stop() noexcept
{
    if (_endpoint) _endpoint->Invalidate();
    if (!_client) return;
    _client->disconnect(this);
    if (!_transportRequest.isEmpty()) _client->Cancel(_transportRequest);
    _client.reset(); // kill/join 后再释放被 helper 借用的文件。
    _image.clear(); _retiringImage.clear(); _transportRequest.clear(); _requester = nullptr; _request = 0;
}

bool OcrProcessor::HandleEvent(const ProcessorEvent& event)
{
    if (!_client) {
        if (event.type == "ocr.command") Send("ocr.result", event.sender, {{"json", QStringLiteral("{\"ok\":false,\"error\":\"OCR stopped\"}")}});
        else if (event.type == ProcessorEvents::Recognize) Send(ProcessorEvents::Failed, event.sender, {{"error", "OCR stopped"}});
        return event.type == "ocr.command" || event.type == ProcessorEvents::Recognize;
    }
    if (event.type == "ocr.command") {
        const auto request = QJsonDocument::fromJson(event.data.value("json").toString().toUtf8()).object();
        const auto command = request.value("command").toString();
        QJsonObject reply;
        if (command == "get_ocr_status") reply = _client->Status();
        else if (command == "ocr_recognize") reply = _client->Submit(request.value("image").toString());
        else if (command == "get_ocr_result") reply = _client->Result(request.value("job_id").toString());
        else reply = _client->Cancel(request.value("job_id").toString());
        Send("ocr.result", event.sender, {{"json", QString::fromUtf8(QJsonDocument(reply).toJson(QJsonDocument::Compact))}});
        return true;
    }
    if (event.type == ProcessorEvents::Cancel && !_request && !_transportRequest.isEmpty() && event.sender == _requester) {
        _client->Cancel(_transportRequest); return true;
    }
    if (event.type != ProcessorEvents::Recognize) return false;
    return Recognize(event, 0);
}

void OcrProcessor::Complete(const QString& type, IProcessorPlugin* receiver, RequestId request, QVariantMap data)
{
    if (request) _endpoint->Respond(request, std::make_shared<ProcessorEvent>(type, nullptr, nullptr, std::move(data)));
    else Send(type, receiver, std::move(data));
}

bool OcrProcessor::Recognize(const ProcessorEvent& event, RequestId request)
{
    if (!_client) { Complete(ProcessorEvents::Failed, event.sender, request, {{"error", "OCR stopped"}}); return true; }
    if (!_transportRequest.isEmpty() || _client->Status().value("state") == QJsonValue("pending")) {
        Complete(ProcessorEvents::Failed, event.sender, request, {{"error", "OCR service is busy"}}); return true;
    }
    _image = qvariant_cast<ImageFilePtr>(event.data.value("image"));
    const auto path = _image ? _image->Path() : event.data.value("path").toString();
    const auto accepted = _client->Submit(path);
    if (!accepted.value("ok").toBool()) {
        _image.clear();
        Complete(ProcessorEvents::Failed, event.sender, request, {{"error", accepted.value("error").toString()}});
    } else {
        _requester = event.sender;
        _request = request;
        // 原生辅助进程的现有传输协议隔离在此处，事件路由不使用该协议标识。
        _transportRequest = accepted.value("job_id").toString();
    }
    return true;
}
