#pragma once

#include "ProcessorEvent.hpp"
#include <QPointer>
#include <functional>

class ProcessorEventLoop;
struct EventEndpointState;
using EndpointId = quint64;
using RequestId = quint64;

struct EventDelivery
{
    enum class Kind { Notification, Request, Cancellation };
    Kind kind = Kind::Notification;
    QString topic; // Notification topic or requested capability.
    ProcessorEventPtr payload;
    RequestId requestId = 0;
    EndpointId sender = 0;
};

enum class RequestStatus
{
    Replied, NoProvider, AmbiguousProvider, Cancelled, TimedOut,
    EndpointUnavailable, HandlerFailed
};

const char* RequestStatusName(RequestStatus status);

struct RequestResult
{
    RequestId requestId = 0;
    RequestStatus status = RequestStatus::Replied;
    ProcessorEventPtr payload; // Domain success/failure belongs in this typed payload.
};

// RAII registration. Creation/registration/invalidation are loop-thread only;
// Notify/Request/Respond/Cancel may be called from workers while this object lives.
class EventEndpoint final
{
public:
    using Handler = std::function<void(const EventDelivery&)>;
    using Completion = std::function<void(const RequestResult&)>;
    ~EventEndpoint();
    EventEndpoint(const EventEndpoint&) = delete;
    EventEndpoint& operator=(const EventEndpoint&) = delete;

    EndpointId Id() const;
    bool Subscribe(const QString& topic);
    bool Provide(const QString& capability);
    bool Activate(); // Inactive endpoints may register, but cannot send/receive.
    void Invalidate(); // Permanent; also withdraws capabilities and cancels work.
    bool Notify(const QString& topic, ProcessorEventPtr payload);
    RequestId Request(const QString& capability, ProcessorEventPtr payload,
                      Completion completion, int timeoutMs = 30000);
    bool Respond(RequestId request, ProcessorEventPtr payload);
    bool Cancel(RequestId request);

private:
    friend class ProcessorEventLoop;
    EventEndpoint(ProcessorEventLoop& loop, std::shared_ptr<EventEndpointState> state);
    QPointer<ProcessorEventLoop> _loop;
    std::shared_ptr<EventEndpointState> _state;
};
