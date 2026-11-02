#include "ImageProcessor.hpp"
#include "ImageFile.hpp"
#include "processor/Events.hpp"
#include "plugin/PluginContext.hpp"
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

ImageProcessor::ImageProcessor(ProcessorEventLoop& loop, std::function<QImage()> clipboard, PluginContext* context)
    : Processor(loop), _clipboard(std::move(clipboard))
{
    EventEndpoint::Handler handler = [this](const EventDelivery& delivery) {
        if (delivery.kind == EventDelivery::Kind::Request && delivery.payload)
            Read(*delivery.payload, delivery.requestId);
        else if (delivery.kind == EventDelivery::Kind::Cancellation && delivery.requestId == _request)
            _cancelled = true;
    };
    if (context) _endpoint = context->CreateEndpoint("image", std::move(handler));
    else {
        _ownedEndpoint = loop.CreateEndpoint("image", std::move(handler));
        _endpoint = _ownedEndpoint.get();
    }
    if (_endpoint) _endpoint->Provide("image.read/v1");
}

ImageProcessor::~ImageProcessor() { Stop(); }
void ImageProcessor::Stop() noexcept
{
    _stopped = true;
    if (_endpoint) _endpoint->Invalidate();
    for (auto* watcher : findChildren<QFutureWatcherBase*>()) {
        watcher->disconnect(this);
        watcher->waitForFinished();
        delete watcher;
    }
    _busy = false; _requester = nullptr; _request = 0;
}

bool ImageProcessor::HandleEvent(const ProcessorEvent& event)
{
    using namespace ProcessorEvents;
    if (event.type == Cancel && !_request && _busy && event.sender == _requester) {
        _cancelled = true; return true;
    }
    if (event.type != ReadImage) return false;
    return Read(event, 0);
}

void ImageProcessor::Complete(const QString& type, IProcessorPlugin* receiver, RequestId request, QVariantMap data)
{
    if (request) _endpoint->Respond(request, std::make_shared<ProcessorEvent>(type, nullptr, nullptr, std::move(data)));
    else Send(type, receiver, std::move(data));
}

bool ImageProcessor::Read(const ProcessorEvent& event, RequestId request)
{
    using namespace ProcessorEvents;
    if (_stopped) { Complete(Failed, event.sender, request, {{"error", "image service stopped"}}); return true; }
    if (_busy) { Complete(Failed, event.sender, request, {{"error", "image service is busy"}}); return true; }
    // GUI 线程访问剪贴板，工作线程负责 PNG 编码。
    const auto image = event.data.contains("image") ? qvariant_cast<QImage>(event.data.value("image")) : _clipboard();
    if (image.isNull()) { Complete(Failed, event.sender, request, {{"error", "clipboard contains no image"}}); return true; }
    _requester = event.sender;
    _request = request;
    _busy = true;
    _cancelled = false;
    auto* watcher = new QFutureWatcher<ImageFilePtr>(this);
    connect(watcher, &QFutureWatcher<ImageFilePtr>::finished, this, [this, watcher] {
        const auto file = watcher->result();
        watcher->deleteLater();
        auto* receiver = _requester;
        const auto request = _request;
        const bool cancelled = _cancelled;
        _busy = false; _requester = nullptr; _request = 0;
        if (cancelled || !file) Complete(ProcessorEvents::Failed, receiver, request,
            {{"error", cancelled ? "image read cancelled" : "image is too large or PNG encoding failed"}});
        else Complete(ProcessorEvents::ImageReady, receiver, request, {{"image", QVariant::fromValue(file)}});
    });
    watcher->setFuture(QtConcurrent::run([image] { return CreateImageFile(image); }));
    return true;
}
