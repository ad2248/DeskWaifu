#include "OcrProcessor.hpp"
#include "processor/Events.hpp"
#include <QJsonArray>

OcrProcessor::OcrProcessor(ProcessorEventLoop& loop)
    : OcrProcessor(loop, std::unique_ptr<OcrClient>(new OcrClient(OcrClient::DefaultExecutable()))) {}

OcrProcessor::OcrProcessor(ProcessorEventLoop& loop, std::unique_ptr<OcrClient> client)
    : Processor(loop), _client(std::move(client))
{
    connect(_client.get(), &OcrClient::WorkerStopped, this, [this] { _retiringImage.clear(); });
    connect(_client.get(), &OcrClient::Finished, this, [this](const QString& request, const QJsonObject& response) {
        if (_transportRequest.isEmpty() || request != _transportRequest) return;
        auto* receiver = _requester;
        _requester = nullptr; _transportRequest.clear();
        if (!response.value("ok").toBool() && _client->Status().value("running").toBool()) _retiringImage = _image;
        _image.clear();
        if (!response.value("ok").toBool()) Send(ProcessorEvents::Failed, receiver, {{"error", response.value("error").toString()}});
        else {
            QStringList texts;
            const auto result = response.value("result").toObject();
            for (const auto& line : result.value("lines").toArray()) texts.append(line.toObject().value("text").toString());
            Send(ProcessorEvents::Recognized, receiver, {{"text", texts.join('\n')}, {"result", result.toVariantMap()}});
        }
    });
}

OcrProcessor::~OcrProcessor()
{
    _client->disconnect(this);
    if (!_transportRequest.isEmpty()) _client->Cancel(_transportRequest);
}

bool OcrProcessor::HandleEvent(const ProcessorEvent& event)
{
    if (event.type == ProcessorEvents::Cancel && !_transportRequest.isEmpty() && event.sender == _requester) {
        _client->Cancel(_transportRequest); return true;
    }
    if (event.type != ProcessorEvents::Recognize) return false;
    if (!_transportRequest.isEmpty() || _client->Status().value("state") == QJsonValue("pending")) {
        Send(ProcessorEvents::Failed, event.sender, {{"error", "OCR service is busy"}}); return true;
    }
    _image = qvariant_cast<ImageFilePtr>(event.data.value("image"));
    const auto path = _image ? _image->Path() : event.data.value("path").toString();
    const auto accepted = _client->Submit(path);
    if (!accepted.value("ok").toBool()) {
        _image.clear();
        Send(ProcessorEvents::Failed, event.sender, {{"error", accepted.value("error").toString()}});
    } else {
        _requester = event.sender;
        // 原生辅助进程的现有传输协议隔离在此处，事件路由不使用该协议标识。
        _transportRequest = accepted.value("job_id").toString();
    }
    return true;
}
