#include "ImageProcessor.hpp"
#include "ImageFile.hpp"
#include "processor/Events.hpp"
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

ImageProcessor::ImageProcessor(ProcessorEventLoop& loop, std::function<QImage()> clipboard)
    : Processor(loop), _clipboard(std::move(clipboard)) {}

bool ImageProcessor::HandleEvent(const ProcessorEvent& event)
{
    using namespace ProcessorEvents;
    if (event.type == Cancel && _busy && event.sender == _requester) {
        _cancelled = true; return true;
    }
    if (event.type != ReadImage) return false;
    if (_busy) { Send(Failed, event.sender, {{"error", "image service is busy"}}); return true; }
    // GUI 线程访问剪贴板，工作线程负责 PNG 编码。
    const auto image = event.data.contains("image") ? qvariant_cast<QImage>(event.data.value("image")) : _clipboard();
    if (image.isNull()) { Send(Failed, event.sender, {{"error", "clipboard contains no image"}}); return true; }
    _requester = event.sender;
    _busy = true;
    _cancelled = false;
    auto* watcher = new QFutureWatcher<ImageFilePtr>(this);
    connect(watcher, &QFutureWatcher<ImageFilePtr>::finished, this, [this, watcher] {
        const auto file = watcher->result();
        watcher->deleteLater();
        auto* receiver = _requester;
        const bool cancelled = _cancelled;
        _busy = false; _requester = nullptr;
        if (cancelled || !file) Send(ProcessorEvents::Failed, receiver,
            {{"error", cancelled ? "image read cancelled" : "image is too large or PNG encoding failed"}});
        else Send(ProcessorEvents::ImageReady, receiver, {{"image", QVariant::fromValue(file)}});
    });
    watcher->setFuture(QtConcurrent::run([image] { return ImageFile::Create(image); }));
    return true;
}
