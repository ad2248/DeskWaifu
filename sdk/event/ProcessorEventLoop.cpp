#include "ProcessorEventLoop.hpp"
#include <QCoreApplication>
#include <QDebug>
#include <QEvent>
#include <QSet>
#include <QThread>
#include <algorithm>
#include <limits>

struct EventEndpointState
{
    EndpointId id;
    QString name;
    std::shared_ptr<EventEndpoint::Handler> handler;
    QSet<QString> subscriptions, capabilities;
    bool valid = true, active = true;
};

const char* RequestStatusName(RequestStatus status)
{
    switch (status) {
    case RequestStatus::Replied: return "replied";
    case RequestStatus::NoProvider: return "no provider";
    case RequestStatus::AmbiguousProvider: return "ambiguous provider";
    case RequestStatus::Cancelled: return "cancelled";
    case RequestStatus::TimedOut: return "timed out";
    case RequestStatus::EndpointUnavailable: return "endpoint unavailable";
    case RequestStatus::HandlerFailed: return "handler failed";
    }
    return "unknown status";
}

namespace {
const auto WakeType = static_cast<QEvent::Type>(QEvent::registerEventType());
bool Ready(const std::shared_ptr<EventEndpointState>& endpoint)
{ return endpoint && endpoint->valid && endpoint->active; }
}

ProcessorEventLoop::ProcessorEventLoop(QObject* parent) : QObject(parent), _timer(this)
{
    _timer.setSingleShot(true);
    connect(&_timer, &QTimer::timeout, this, [this] { ExpireRequests(); });
}

ProcessorEventLoop::~ProcessorEventLoop()
{
    for (auto& entry : _endpoints) entry.second->valid = false;
}

bool ProcessorEventLoop::Register(IProcessorPlugin* processor)
{
    if (QThread::currentThread() != thread()) return false;
    std::lock_guard<std::mutex> lock(_mutex);
    if (!processor || std::any_of(_processors.begin(), _processors.end(), [processor](const auto& entry) { return entry->processor == processor; })) return false;
    _processors.push_back(std::make_shared<Registration>(Registration{processor}));
    return true;
}

void ProcessorEventLoop::Unregister(IProcessorPlugin* processor)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!processor) return;
    std::lock_guard<std::mutex> lock(_mutex);
    _processors.erase(std::remove_if(_processors.begin(), _processors.end(), [processor](const auto& entry) {
        if (entry->processor != processor) return false;
        entry->active = false;
        return true;
    }), _processors.end());
}

bool ProcessorEventLoop::Contains(IProcessorPlugin* processor) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return std::any_of(_processors.begin(), _processors.end(), [processor](const auto& entry) { return entry->processor == processor; });
}

void ProcessorEventLoop::WakeLocked()
{
    if (!_scheduled && !_dispatching && !_events.empty()) {
        _scheduled = true;
        QCoreApplication::postEvent(this, new QEvent(WakeType));
    }
}

bool ProcessorEventLoop::Post(ProcessorEvent event)
{ return PostEvent(std::make_shared<ProcessorEvent>(std::move(event))); }

bool ProcessorEventLoop::PostEvent(ProcessorEventPtr event)
{
    if (!event || event->type.isEmpty()) return false;
    std::lock_guard<std::mutex> lock(_mutex);
    const auto find = [this](IProcessorPlugin* pointer) -> std::shared_ptr<Registration> {
        for (const auto& entry : _processors) if (entry->processor == pointer) return entry;
        return {};
    };
    const auto sender = find(event->sender), receiver = find(event->receiver);
    if ((event->sender && !sender) || (event->receiver && !receiver)) return false;
    _events.push_back([this, event, sender, receiver] {
        std::vector<std::shared_ptr<Registration>> processors;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            processors = _processors;
        }
        for (const auto& entry : processors) {
            bool deliver;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                if ((sender && !sender->active) || (receiver && !receiver->active)) break;
                deliver = entry->active && (!receiver || receiver == entry);
            }
            if (deliver && entry->processor->HandleEvent(*event)) break;
        }
    });
    WakeLocked();
    return true;
}

std::unique_ptr<EventEndpoint> ProcessorEventLoop::CreateEndpoint(const QString& name, EventEndpoint::Handler handler, bool active)
{
    if (QThread::currentThread() != thread() || name.isEmpty() || !handler) return {};
    std::lock_guard<std::mutex> lock(_mutex);
    for (const auto& entry : _endpoints) if (entry.second->name == name) return {};
    auto state = std::make_shared<EventEndpointState>();
    state->id = _nextEndpoint++;
    state->name = name;
    state->handler = std::make_shared<EventEndpoint::Handler>(std::move(handler));
    state->active = active;
    _endpoints.emplace(state->id, state);
    return std::unique_ptr<EventEndpoint>(new EventEndpoint(*this, std::move(state)));
}

bool ProcessorEventLoop::RegisterTopic(const std::shared_ptr<EventEndpointState>& endpoint, const QString& topic, bool capability)
{
    if (QThread::currentThread() != thread() || topic.isEmpty()) return false;
    std::lock_guard<std::mutex> lock(_mutex);
    if (!endpoint->valid) return false;
    auto& topics = capability ? endpoint->capabilities : endpoint->subscriptions;
    if (topics.contains(topic)) return false;
    topics.insert(topic);
    return true;
}

bool ProcessorEventLoop::Activate(const std::shared_ptr<EventEndpointState>& endpoint)
{
    if (QThread::currentThread() != thread()) return false;
    std::lock_guard<std::mutex> lock(_mutex);
    if (!endpoint->valid) return false;
    endpoint->active = true;
    return true;
}

void ProcessorEventLoop::Invalidate(const std::shared_ptr<EventEndpointState>& endpoint)
{
    Q_ASSERT(QThread::currentThread() == thread());
    // Release captures outside the mutex. A currently executing handler holds
    // its own copy, so self-invalidation is also safe.
    std::shared_ptr<EventEndpoint::Handler> retired;
    std::lock_guard<std::mutex> lock(_mutex);
    if (!endpoint->valid) return;
    endpoint->valid = false;
    retired = std::move(endpoint->handler);
    _endpoints.erase(endpoint->id);
    for (auto it = _requests.begin(); it != _requests.end();) {
        const auto id = it->first;
        const bool affected = it->second.sender == endpoint || it->second.receiver == endpoint;
        ++it;
        if (affected) FinishLocked(id, RequestStatus::EndpointUnavailable);
    }
    WakeLocked();
}

void ProcessorEventLoop::QueueDeliveryLocked(const std::shared_ptr<EventEndpointState>& receiver,
                                            EventDelivery delivery, const std::shared_ptr<EventEndpointState>& sender)
{
    _events.push_back([this, receiver, sender, delivery = std::move(delivery)] {
        std::shared_ptr<EventEndpoint::Handler> handler;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!Ready(receiver) || (sender && !Ready(sender))) return;
            handler = receiver->handler;
            if (delivery.kind == EventDelivery::Kind::Request) {
                const auto pending = _requests.find(delivery.requestId);
                if (pending == _requests.end() || pending->second.receiver != receiver) return;
                if (pending->second.deadline <= std::chrono::steady_clock::now()) {
                    FinishLocked(delivery.requestId, RequestStatus::TimedOut);
                    return;
                }
            }
        }
        try { (*handler)(delivery); }
        catch (...) {
            qWarning() << "Event endpoint threw:" << receiver->name;
            if (delivery.kind == EventDelivery::Kind::Request) {
                std::lock_guard<std::mutex> lock(_mutex);
                FinishLocked(delivery.requestId, RequestStatus::HandlerFailed);
            }
        }
    });
}

void ProcessorEventLoop::QueueCompletionLocked(const std::shared_ptr<EventEndpointState>& sender,
                                              EventEndpoint::Completion completion, RequestResult result)
{
    _events.push_back([this, sender, completion = std::move(completion), result = std::move(result)] {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!Ready(sender)) return;
        }
        completion(result);
    });
}

bool ProcessorEventLoop::Notify(const std::shared_ptr<EventEndpointState>& sender, const QString& topic, ProcessorEventPtr payload)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!Ready(sender) || topic.isEmpty() || !payload) return false;
    size_t recipients = 0;
    for (const auto& entry : _endpoints)
        if (Ready(entry.second) && entry.second->subscriptions.contains(topic)) ++recipients;
    if (recipients && (_events.size() >= MaxQueuedDeliveries || recipients > MaxQueuedDeliveries - _events.size())) return false;
    for (const auto& entry : _endpoints) {
        if (Ready(entry.second) && entry.second->subscriptions.contains(topic))
            QueueDeliveryLocked(entry.second, {EventDelivery::Kind::Notification, topic, payload, 0, sender->id}, sender);
    }
    WakeLocked();
    return true;
}

RequestId ProcessorEventLoop::Request(const std::shared_ptr<EventEndpointState>& sender, const QString& capability,
                                     ProcessorEventPtr payload, EventEndpoint::Completion completion, int timeoutMs)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!Ready(sender) || capability.isEmpty() || !payload || !completion || timeoutMs <= 0 ||
        _events.size() >= MaxQueuedDeliveries || _requests.size() >= MaxPendingRequests) return 0;
    const auto id = _nextRequest++;
    std::shared_ptr<EventEndpointState> provider;
    int count = 0;
    for (const auto& entry : _endpoints) {
        if (Ready(entry.second) && entry.second->capabilities.contains(capability)) { provider = entry.second; ++count; }
    }
    if (count != 1) {
        QueueCompletionLocked(sender, std::move(completion), {id, count ? RequestStatus::AmbiguousProvider : RequestStatus::NoProvider, {}});
    } else {
        _requests.emplace(id, PendingRequest{sender, provider, capability, std::move(completion),
                          std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs)});
        QueueDeliveryLocked(provider, {EventDelivery::Kind::Request, capability, std::move(payload), id, sender->id}, sender);
    }
    WakeLocked();
    return id;
}

void ProcessorEventLoop::FinishLocked(RequestId id, RequestStatus status, ProcessorEventPtr payload)
{
    const auto it = _requests.find(id);
    if (it == _requests.end()) return;
    auto pending = std::move(it->second);
    _requests.erase(it);
    if (status != RequestStatus::Replied && Ready(pending.receiver))
        QueueDeliveryLocked(pending.receiver, {EventDelivery::Kind::Cancellation, pending.capability, {}, id, pending.sender->id});
    // Even an invalid requester's captures are retired on the dispatch thread,
    // outside the mutex (their destructors may unregister other endpoints).
    QueueCompletionLocked(pending.sender, std::move(pending.completion), {id, status, std::move(payload)});
}

bool ProcessorEventLoop::Respond(const std::shared_ptr<EventEndpointState>& receiver, RequestId id, ProcessorEventPtr payload)
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto it = _requests.find(id);
    if (!Ready(receiver) || !payload || it == _requests.end() || it->second.receiver != receiver) return false;
    const bool late = it->second.deadline <= std::chrono::steady_clock::now();
    FinishLocked(id, late ? RequestStatus::TimedOut : RequestStatus::Replied, late ? ProcessorEventPtr{} : std::move(payload));
    WakeLocked();
    return !late;
}

bool ProcessorEventLoop::Cancel(const std::shared_ptr<EventEndpointState>& sender, RequestId id)
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto it = _requests.find(id);
    if (!Ready(sender) || it == _requests.end() || it->second.sender != sender) return false;
    FinishLocked(id, RequestStatus::Cancelled);
    WakeLocked();
    return true;
}

void ProcessorEventLoop::ArmTimerLocked()
{
    _timer.stop();
    if (_requests.empty()) return;
    auto deadline = std::chrono::steady_clock::time_point::max();
    for (const auto& entry : _requests) deadline = std::min(deadline, entry.second.deadline);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    _timer.start(static_cast<int>(std::max<qint64>(1, std::min<qint64>(ms + 1, std::numeric_limits<int>::max()))));
}

void ProcessorEventLoop::ExpireRequests()
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto now = std::chrono::steady_clock::now();
    for (auto it = _requests.begin(); it != _requests.end();) {
        const auto id = it->first;
        const bool expired = it->second.deadline <= now;
        ++it;
        if (expired) FinishLocked(id, RequestStatus::TimedOut);
    }
    ArmTimerLocked();
    WakeLocked();
}

bool ProcessorEventLoop::event(QEvent* wake)
{
    if (wake->type() != WakeType) return QObject::event(wake);
    std::function<void()> next;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _scheduled = false;
        if (_dispatching || _events.empty()) return true;
        next = std::move(_events.front());
        _events.pop_front();
        _dispatching = true;
    }
    try { next(); }
    catch (...) { qWarning() << "Event callback threw; continuing event dispatch"; }
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _dispatching = false;
    }
    // A busy FIFO must not keep rearming an already-expired deadline forever.
    ExpireRequests();
    return true;
}

EventEndpoint::EventEndpoint(ProcessorEventLoop& loop, std::shared_ptr<EventEndpointState> state)
    : _loop(&loop), _state(std::move(state)) {}
EventEndpoint::~EventEndpoint() { Invalidate(); }
EndpointId EventEndpoint::Id() const { return _state->id; }
bool EventEndpoint::Subscribe(const QString& topic) { return _loop && _loop->RegisterTopic(_state, topic, false); }
bool EventEndpoint::Provide(const QString& capability) { return _loop && _loop->RegisterTopic(_state, capability, true); }
bool EventEndpoint::Activate() { return _loop && _loop->Activate(_state); }
void EventEndpoint::Invalidate() { if (_loop) _loop->Invalidate(_state); }
bool EventEndpoint::Notify(const QString& topic, ProcessorEventPtr payload)
{ return _loop && _loop->Notify(_state, topic, std::move(payload)); }
RequestId EventEndpoint::Request(const QString& capability, ProcessorEventPtr payload, Completion completion, int timeoutMs)
{ return _loop ? _loop->Request(_state, capability, std::move(payload), std::move(completion), timeoutMs) : 0; }
bool EventEndpoint::Respond(RequestId request, ProcessorEventPtr payload)
{ return _loop && _loop->Respond(_state, request, std::move(payload)); }
bool EventEndpoint::Cancel(RequestId request) { return _loop && _loop->Cancel(_state, request); }
