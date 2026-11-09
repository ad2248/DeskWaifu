// 定义端点消息、请求结果及事件路由器的 RAII 端点接口。
#pragma once

#include "ProcessorEvent.hpp"
#include <QPointer>
#include <functional>

class ProcessorEventLoop;
struct EventEndpointState;
using EndpointId = quint64;
using RequestId = quint64;

// 端点处理回调收到的通知、请求或取消消息；负载共享所有权并保留动态类型。
// 取消消息不携带负载，以 requestId 关联原请求。
struct EventDelivery
{
    enum class Kind { Notification, Request, Cancellation };
    Kind kind = Kind::Notification;
    QString topic; // Notification topic or requested capability.
    ProcessorEventPtr payload;
    RequestId requestId = 0;
    EndpointId sender = 0;
};

// 请求路由的终结状态；Replied 仅表示收到响应，不代表领域操作成功。
enum class RequestStatus
{
    Replied, NoProvider, AmbiguousProvider, Cancelled, TimedOut,
    EndpointUnavailable, HandlerFailed
};

// 返回静态存储的状态描述，未知枚举值返回 unknown status；调用者不得释放。
const char* RequestStatusName(RequestStatus status);

// 异步请求的最终路由结果；仅正常响应时携带提供者返回的负载。
struct RequestResult
{
    RequestId requestId = 0;
    RequestStatus status = RequestStatus::Replied;
    ProcessorEventPtr payload; // Domain success/failure belongs in this typed payload.
};

// 以 RAII 管理端点注册；创建、订阅、提供能力、激活、失效和析构均在路由器所属线程执行。
// Notify、Request、Respond、Cancel 可从工作线程调用，但调用期间端点和路由器必须存活。
// 处理回调和完成回调在路由器所属线程执行，端点失效后跳过尚未执行的回调。
class EventEndpoint final
{
public:
    using Handler = std::function<void(const EventDelivery&)>;
    using Completion = std::function<void(const RequestResult&)>;
    ~EventEndpoint();
    EventEndpoint(const EventEndpoint&) = delete;
    EventEndpoint& operator=(const EventEndpoint&) = delete;

    // 返回端点在该路由器内的稳定标识。
    EndpointId Id() const;
    // 登记通知订阅；主题为空、重复登记、端点失效或线程不符时返回 false。
    bool Subscribe(const QString& topic);
    // 登记请求能力；可有多个提供者，但请求仅在恰有一个就绪提供者时派发。
    // 能力为空、重复登记、端点失效或线程不符时返回 false。
    bool Provide(const QString& capability);
    // 激活收发；未激活时仅可登记。端点失效、路由器已销毁或线程不符时返回 false。
    bool Activate(); // Inactive endpoints may register, but cannot send/receive.
    // 永久撤销订阅与能力，并终结相关请求；可重复调用。
    void Invalidate(); // Permanent; also withdraws capabilities and cancels work.
    // 向就绪订阅者排队通知，无订阅者也返回 true；参数无效、端点未就绪或容量不足返回 false。
    // 返回值仅表示接收入队；失效的发送者或接收者会使后续投递被跳过。
    bool Notify(const QString& topic, ProcessorEventPtr payload);
    // 向唯一就绪提供者发起请求，timeoutMs 必须为正，completion 必须有效。
    // 参数无效、端点未就绪或容量不足返回 0；否则返回 ID，并异步完成（包括无提供者或歧义）。
    RequestId Request(const QString& capability, ProcessorEventPtr payload,
                      Completion completion, int timeoutMs = 30000);
    // 响应本端点承接的请求；成功返回 true，端点未就绪、负载为空、
    // 请求不属本端点、已终结或超时返回 false。
    bool Respond(RequestId request, ProcessorEventPtr payload);
    // 取消本端点发起的未终结请求，成功返回 true；不保证提供者的工作已停止。
    // 端点未就绪、请求不存在或不属本端点时返回 false。
    bool Cancel(RequestId request);

private:
    friend class ProcessorEventLoop;
    EventEndpoint(ProcessorEventLoop& loop, std::shared_ptr<EventEndpointState> state);
    QPointer<ProcessorEventLoop> _loop;
    std::shared_ptr<EventEndpointState> _state;
};
