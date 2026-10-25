#include "IpcProcessor.hpp"
#include "processor/Events.hpp"
#include <QPointer>

IpcProcessor::IpcProcessor(ProcessorEventLoop& loop, LAppIPC& ipc) : Processor(loop), _ipc(ipc)
{
    QPointer<IpcProcessor> self(this);
    _ipc.SetReadyCallback([self] { if (self) self->Schedule(); });
    Refresh();
}

void IpcProcessor::Schedule()
{
    if (_queued || _paused) return;
    _queued = true;
    for (const auto& notifier : _notifiers) notifier->setEnabled(false);
    Send(ProcessorEvents::IpcReady, this);
}

void IpcProcessor::SetPaused(bool paused)
{
    _paused = paused;
    if (paused) for (const auto& notifier : _notifiers) notifier->setEnabled(false);
    else Schedule();
}

void IpcProcessor::Refresh()
{
    _notifiers.clear();
    auto add = [this](int fd, QSocketNotifier::Type type) {
        auto notifier = std::make_unique<QSocketNotifier>(fd, type);
        connect(notifier.get(), &QSocketNotifier::activated, this, [this] { Schedule(); });
        _notifiers.push_back(std::move(notifier));
    };
    for (int fd : _ipc.ReadDescriptors()) add(fd, QSocketNotifier::Read);
    for (int fd : _ipc.WriteDescriptors()) add(fd, QSocketNotifier::Write);
    if (_ipc.HasBufferedWork()) Schedule();
}

bool IpcProcessor::HandleEvent(const ProcessorEvent& event)
{
    if (event.type != ProcessorEvents::IpcReady) return false;
    _queued = false;
    if (_paused) return true;
    _ipc.Poll();
    Refresh();
    return true;
}
