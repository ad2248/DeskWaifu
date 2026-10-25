#pragma once

#include "ProcessorEvent.hpp"
#include "EventEndpoint.hpp"
#include <QObject>
#include <QTimer>
#include <chrono>
#include <deque>
#include <mutex>
#include <memory>
#include <vector>
#include <map>
#include <type_traits>

// Qt 提供等待和唤醒机制。此对象在所属 GUI 线程上按顺序、非递归地派发事件，
// 不另行实现 exec 循环。
class ProcessorEventLoop : public QObject
{
public:
    static constexpr size_t MaxQueuedDeliveries = 4096;
    static constexpr size_t MaxPendingRequests = 1024;
    explicit ProcessorEventLoop(QObject* parent = nullptr);
    ~ProcessorEventLoop() override;
    bool Register(IProcessor* processor);
    void Unregister(IProcessor* processor);
    bool Post(ProcessorEvent event); // 线程安全；指针生命周期仍由生产者负责。
    bool PostEvent(ProcessorEventPtr event); // Preserves derived events, including through a base shared_ptr.
    template<class T, typename std::enable_if<std::is_base_of<ProcessorEvent, T>::value &&
                                             !std::is_same<ProcessorEvent, T>::value, int>::type = 0>
    bool Post(T event) { return PostEvent(std::make_shared<T>(std::move(event))); }
    bool Contains(IProcessor* processor) const;
    std::unique_ptr<EventEndpoint> CreateEndpoint(const QString& name, EventEndpoint::Handler handler,
                                                 bool active = true);

protected:
    bool event(QEvent* event) override;

private:
    friend class EventEndpoint;
    struct Registration { IProcessor* processor; bool active = true; };
    struct PendingRequest
    {
        std::shared_ptr<EventEndpointState> sender, receiver;
        QString capability;
        EventEndpoint::Completion completion;
        std::chrono::steady_clock::time_point deadline;
    };
    bool RegisterTopic(const std::shared_ptr<EventEndpointState>& endpoint, const QString& topic, bool capability);
    bool Activate(const std::shared_ptr<EventEndpointState>& endpoint);
    void Invalidate(const std::shared_ptr<EventEndpointState>& endpoint);
    bool Notify(const std::shared_ptr<EventEndpointState>& sender, const QString& topic, ProcessorEventPtr payload);
    RequestId Request(const std::shared_ptr<EventEndpointState>& sender, const QString& capability,
                      ProcessorEventPtr payload, EventEndpoint::Completion completion, int timeoutMs);
    bool Respond(const std::shared_ptr<EventEndpointState>& receiver, RequestId id, ProcessorEventPtr payload);
    bool Cancel(const std::shared_ptr<EventEndpointState>& sender, RequestId id);
    void FinishLocked(RequestId id, RequestStatus status, ProcessorEventPtr payload = {});
    void QueueDeliveryLocked(const std::shared_ptr<EventEndpointState>& receiver, EventDelivery delivery,
                             const std::shared_ptr<EventEndpointState>& sender = {});
    void QueueCompletionLocked(const std::shared_ptr<EventEndpointState>& sender,
                               EventEndpoint::Completion completion, RequestResult result);
    void ExpireRequests();
    void ArmTimerLocked();
    void WakeLocked();
    mutable std::mutex _mutex;
    std::vector<std::shared_ptr<Registration>> _processors;
    std::map<EndpointId, std::shared_ptr<EventEndpointState>> _endpoints;
    std::map<RequestId, PendingRequest> _requests;
    std::deque<std::function<void()>> _events; // Both legacy and endpoint delivery use this FIFO.
    EndpointId _nextEndpoint = 1;
    RequestId _nextRequest = 1;
    QTimer _timer;
    bool _scheduled = false, _dispatching = false;
};
