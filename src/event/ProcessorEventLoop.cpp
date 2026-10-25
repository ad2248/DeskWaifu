#include "ProcessorEventLoop.hpp"
#include <QCoreApplication>
#include <QEvent>
#include <QThread>
#include <algorithm>

namespace {
const auto WakeType = static_cast<QEvent::Type>(QEvent::registerEventType());
}

bool ProcessorEventLoop::Register(IProcessor* processor)
{
    Q_ASSERT(QThread::currentThread() == thread());
    std::lock_guard<std::mutex> lock(_mutex);
    if (!processor || std::any_of(_processors.begin(), _processors.end(), [processor](const auto& entry) { return entry->processor == processor; })) return false;
    _processors.push_back(std::make_shared<Registration>(Registration{processor}));
    return true;
}

void ProcessorEventLoop::Unregister(IProcessor* processor)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!processor) return;
    std::lock_guard<std::mutex> lock(_mutex);
    _processors.erase(std::remove_if(_processors.begin(), _processors.end(), [processor](const auto& entry) {
        if (entry->processor != processor) return false;
        entry->active = false;
        return true;
    }), _processors.end());
    _events.erase(std::remove_if(_events.begin(), _events.end(), [processor](const ProcessorEvent& event) {
        return event.sender == processor || event.receiver == processor;
    }), _events.end());
}

bool ProcessorEventLoop::Contains(IProcessor* processor) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return std::any_of(_processors.begin(), _processors.end(), [processor](const auto& entry) { return entry->processor == processor; });
}

bool ProcessorEventLoop::ValidLocked(const ProcessorEvent& event) const
{
    const auto valid = [this](IProcessor* processor) {
        return !processor || std::any_of(_processors.begin(), _processors.end(), [processor](const auto& entry) { return entry->processor == processor; });
    };
    return valid(event.sender) && valid(event.receiver);
}

void ProcessorEventLoop::WakeLocked()
{
    if (!_scheduled && !_dispatching && !_events.empty()) {
        _scheduled = true;
        QCoreApplication::postEvent(this, new QEvent(WakeType));
    }
}

bool ProcessorEventLoop::Post(ProcessorEvent event)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (event.type.isEmpty() || !ValidLocked(event)) return false;
    _events.push_back(std::move(event));
    WakeLocked();
    return true;
}

bool ProcessorEventLoop::event(QEvent* wake)
{
    if (wake->type() != WakeType) return QObject::event(wake);
    ProcessorEvent next;
    std::vector<std::shared_ptr<Registration>> processors;
    std::shared_ptr<Registration> sender;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _scheduled = false;
        if (_dispatching || _events.empty()) return true;
        next = std::move(_events.front());
        _events.pop_front();
        processors = _processors;
        for (const auto& entry : processors) if (entry->processor == next.sender) sender = entry;
        _dispatching = true;
    }
    for (const auto& entry : processors) {
        bool deliver;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!ValidLocked(next) || (sender && !sender->active)) break;
            deliver = entry->active && (!next.receiver || next.receiver == entry->processor);
        }
        if (deliver && entry->processor->HandleEvent(next)) break;
    }
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _dispatching = false;
        WakeLocked();
    }
    return true;
}
