#pragma once

#include <QObject>
#include <QTimer>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

#include "EventEndpoint.hpp"
#include "ProcessorEvent.hpp"
#include "plugin/IProcessorPlugin.hpp"

// 通过 Qt 事件循环路由旧式处理器事件和端点消息。
//
// 在所属 GUI 线程上按 FIFO 顺序、非递归地执行处理器和端点回调，
// 等待和唤醒由 Qt 提供，不另行实现 exec 循环。回调执行期间不持有 _mutex。
// 注册、注销和端点生命周期操作必须在所属线程执行；事件投递和端点的
// Notify、Request、Respond、Cancel 可从工作线程调用，但调用期间对象必须存活。
class ProcessorEventLoop : public QObject {
public:
    // 端点通知和请求入队时使用的容量限制；旧式 Post 不受此限制。
    static constexpr size_t MaxQueuedDeliveries = 4096;
    // 等待响应的请求数上限，不包含已排队的完成回调。
    static constexpr size_t MaxPendingRequests = 1024;

    // 在当前线程创建路由器，parent 遵循 QObject 的父子对象所有权规则。
    explicit ProcessorEventLoop(QObject* parent = nullptr);
    // 使所有端点失效；不再派发尚未执行的消息和完成回调。
    ~ProcessorEventLoop() override;

    // 注册非拥有的处理器指针。线程不符、指针为空或重复注册时返回 false。
    // 处理器必须在销毁前于所属线程注销。
    bool Register(IProcessorPlugin* processor);
    // 在所属线程注销处理器，使涉及该注册的排队事件不再向其派发。
    void Unregister(IProcessorPlugin* processor);

    // 线程安全地投递基类事件，返回是否成功入队，而非是否已被处理。
    // 事件中的裸指针生命周期仍由生产者负责；拒绝条件同 PostEvent。
    bool Post(ProcessorEvent event);
    // 线程安全地共享事件所有权并保留动态类型，包括通过基类共享指针传入的事件。
    // 事件为空、类型为空或非空的发送者/接收者未注册时返回 false。
    // 指定接收者时定向派发，否则按注册顺序派发，直到处理器返回 true。
    bool PostEvent(ProcessorEventPtr event);
    // 按值接收派生事件并转交 PostEvent，避免切片；线程约束和返回值同 PostEvent。
    template <class T, typename std::enable_if<std::is_base_of<ProcessorEvent, T>::value && !std::is_same<ProcessorEvent, T>::value, int>::type = 0> bool Post(T event)
    {
        return PostEvent(std::make_shared<T>(std::move(event)));
    }
    // 线程安全地查询处理器是否已注册；不保证返回后处理器仍然存活。
    bool Contains(IProcessorPlugin* processor) const;

    // 在所属线程创建端点，将所有权交给调用者；销毁端点会使其永久失效。
    // name 必须非空且在现存端点中唯一，handler 必须有效，否则返回空指针；
    // 在其他线程调用也返回空指针。handler 在路由器所属线程执行。
    // active 为 false 时可登记订阅和能力，但激活前不能收发消息。
    std::unique_ptr<EventEndpoint> CreateEndpoint(const QString& name, EventEndpoint::Handler handler, bool active = true);

protected:
    // 每次内部唤醒最多执行一个排队回调，防止嵌套事件循环递归派发。
    // 内部唤醒返回 true，其他事件交给 QObject 处理。
    bool event(QEvent* event) override;

private:
    friend class EventEndpoint;
    // 排队事件共享注册状态，以便注销后跳过投递；不拥有处理器。
    struct Registration {
        IProcessorPlugin* processor;
        bool active = true;
    };
    // 保存唯一提供者、完成回调和单调时钟截止时间，直至请求终结。
    struct PendingRequest {
        std::shared_ptr<EventEndpointState> sender, receiver;
        QString capability;
        EventEndpoint::Completion completion;
        std::chrono::steady_clock::time_point deadline;
    };
    // 在所属线程登记通知订阅或请求能力，由 capability 选择后者。
    // 线程不符、主题为空、端点失效或重复登记时返回 false。
    bool RegisterTopic(const std::shared_ptr<EventEndpointState>& endpoint, const QString& topic, bool capability);
    // 在所属线程激活端点；线程不符或端点已失效时返回 false。
    bool Activate(const std::shared_ptr<EventEndpointState>& endpoint);
    // 在所属线程永久移除端点，并以 EndpointUnavailable 终结相关请求。
    void Invalidate(const std::shared_ptr<EventEndpointState>& endpoint);

    // 线程安全地向所有就绪订阅者排队通知。无订阅者也视为成功。
    // 发送者未就绪、主题或负载无效、投递容量不足时返回 false，不进行部分投递。
    bool Notify(const std::shared_ptr<EventEndpointState>& sender, const QString& topic, ProcessorEventPtr payload);
    // 线程安全地向唯一就绪能力提供者请求服务，timeoutMs 必须为正毫秒数。
    // 发送者未就绪、参数无效或容量不足时返回 0；否则返回请求 ID。
    // 无提供者或存在多个提供者时也返回 ID，并异步回报相应状态。
    // completion 在所属线程执行；执行前发送者失效时跳过回调。
    RequestId Request(const std::shared_ptr<EventEndpointState>& sender, const QString& capability, ProcessorEventPtr payload, EventEndpoint::Completion completion, int timeoutMs);
    // 线程安全地响应 receiver 所承接的请求。端点未就绪、负载为空、
    // 请求不存在或归属不符时返回 false；迟到响应以 TimedOut 终结并返回 false。
    bool Respond(const std::shared_ptr<EventEndpointState>& receiver, RequestId id, ProcessorEventPtr payload);
    // 线程安全地取消 sender 发起的请求。端点未就绪、请求不存在或归属不符
    // 时返回 false，否则以 Cancelled 终结请求并返回 true。
    bool Cancel(const std::shared_ptr<EventEndpointState>& sender, RequestId id);
    // 以下名称以 Locked 结尾的方法均要求调用者已持有 _mutex。

    // 移除待处理请求并排队完成回调；非正常响应还会向就绪提供者排队取消通知。
    // 请求不存在时无操作；本方法不负责唤醒队列。
    void FinishLocked(RequestId id, RequestStatus status, ProcessorEventPtr payload = { });
    // 排队投递，在执行时复查端点状态和请求期限；不在此处检查容量或唤醒。
    // sender 为空时只检查接收者，供取消通知使用。
    void QueueDeliveryLocked(const std::shared_ptr<EventEndpointState>& receiver, EventDelivery delivery, const std::shared_ptr<EventEndpointState>& sender = { });
    // 排队完成回调，仅在发送者仍就绪时执行；不在此处检查容量或唤醒。
    void QueueCompletionLocked(const std::shared_ptr<EventEndpointState>& sender, EventEndpoint::Completion completion, RequestResult result);
    // 在所属线程终结已超时请求，更新定时器并安排后续派发；内部获取锁。
    void ExpireRequests();
    // 在所属线程将单次定时器设为最近的请求期限；无待处理请求则停止定时器。
    void ArmTimerLocked();
    // 队列非空且没有待处理唤醒或正在执行的派发时，向 Qt 投递一次唤醒。
    void WakeLocked();

    // 保护注册表、端点与请求状态、共享队列以及唤醒/派发标志。
    mutable std::mutex _mutex;
    std::vector<std::shared_ptr<Registration>> _processors;
    std::map<EndpointId, std::shared_ptr<EventEndpointState>> _endpoints;
    std::map<RequestId, PendingRequest> _requests;
    // 旧式事件、端点投递和请求完成回调共用的 FIFO。
    std::deque<std::function<void()>> _events;
    EndpointId _nextEndpoint = 1;
    RequestId _nextRequest = 1;
    // 仅在所属线程操作，用于检查最近的请求期限。
    QTimer _timer;
    // 分别表示已投递 Qt 唤醒事件、正在执行一个队列回调。
    bool _scheduled = false, _dispatching = false;
};
