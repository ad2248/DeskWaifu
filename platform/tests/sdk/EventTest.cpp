#include "event/ProcessorEventLoop.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Drain() { for (int i = 0; i < 80; ++i) QCoreApplication::processEvents(); }
void Wait(const std::function<bool()>& done)
{
    QElapsedTimer timer; timer.start();
    while (!done() && timer.elapsed() < 2000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    Check(done(), "asynchronous operation timed out");
}
struct Value : ProcessorEvent
{
    explicit Value(int value) : ProcessorEvent("value"), value(value) {}
    int value;
};
struct Unknown : ProcessorEvent {};
struct Handler : IProcessorPlugin
{
    std::function<bool(const ProcessorEvent&)> callback;
    bool Initialize(PluginContext&, QString*) override { return true; }
    void Stop() noexcept override {}
    bool HandleEvent(const ProcessorEvent& event) override { return callback(event); }
};
}

int main(int argc, char** argv)
try {
    QCoreApplication app(argc, argv);
    ProcessorEventLoop loop;
    Handler legacy;
    int legacyValue = 0;
    legacy.callback = [&](const ProcessorEvent& base) {
        const auto* value = dynamic_cast<const Value*>(&base);
        if (!value) return false;
        legacyValue = value->value;
        return true;
    };
    Check(loop.Register(&legacy), "register legacy");
    ProcessorEventPtr base = std::make_shared<Value>(42);
    Check(loop.PostEvent(base), "post derived through shared base");
    Drain();
    Check(legacyValue == 42, "shared base must preserve dynamic type");
    Check(loop.Post(Value(43)), "typed Post must not slice");
    Drain(); Check(legacyValue == 43, "typed Post preserves dynamic type");
    Check(loop.Post({"unknown"}), "unknown legacy events accepted"); Drain();

    int notificationsA = 0, notificationsB = 0, requests = 0, cancellations = 0;
    bool autoReply = true;
    EventEndpoint* service = nullptr;
    auto provider = loop.CreateEndpoint("provider", [&](const EventDelivery& message) {
        Check(QThread::currentThread() == app.thread(), "main-thread endpoint delivery");
        if (message.kind == EventDelivery::Kind::Cancellation) { ++cancellations; return; }
        if (message.kind == EventDelivery::Kind::Notification) { ++notificationsA; return; }
        ++requests;
        const auto* value = dynamic_cast<const Value*>(message.payload.get());
        Check(value && value->value == 42, "typed request must not slice");
        if (autoReply) Check(service->Respond(message.requestId, std::make_shared<Value>(84)), "provider replies");
    });
    service = provider.get();
    auto observer = loop.CreateEndpoint("observer", [&](const EventDelivery&) { ++notificationsB; });
    auto client = loop.CreateEndpoint("client", [](const EventDelivery&) {});
    Check(provider && observer && client, "create endpoints");
    Check(!loop.CreateEndpoint("provider", [](const EventDelivery&) {}), "duplicate endpoint name rejected");
    Check(provider->Subscribe("changed") && observer->Subscribe("changed"), "subscribe two observers");
    Check(!provider->Subscribe("changed"), "duplicate subscription rejected");
    Check(provider->Provide("compute") && !provider->Provide("compute"), "duplicate capability rejected");
    Check(client->Notify("changed", base), "notify");
    Check(notificationsA == 0 && notificationsB == 0, "notification asynchronous");
    Drain();
    Check(notificationsA == 1 && notificationsB == 1, "notification reaches every subscriber");
    Check(client->Notify("unknown-topic", std::make_shared<Unknown>()), "unknown notification is harmless");
    Drain(); Check(notificationsA == 1 && notificationsB == 1, "unknown topic has no subscribers");

    std::vector<RequestResult> results;
    auto complete = [&](const RequestResult& result) { results.push_back(result); };
    auto missing = client->Request("missing", base, complete);
    Check(missing && results.empty(), "missing provider completion asynchronous");
    Drain(); Check(results.back().requestId == missing && results.back().status == RequestStatus::NoProvider, "missing capability explicit");
    auto second = loop.CreateEndpoint("second", [](const EventDelivery&) {});
    Check(second->Provide("compute"), "second provider allowed for explicit ambiguity");
    auto ambiguous = client->Request("compute", base, complete);
    Drain(); Check(results.back().requestId == ambiguous && results.back().status == RequestStatus::AmbiguousProvider && requests == 0, "ambiguity never chooses by registration order");
    second.reset();
    auto request = client->Request("compute", base, complete);
    Drain();
    Check(results.back().requestId == request && results.back().status == RequestStatus::Replied, "response correlation");
    Check(dynamic_cast<const Value*>(results.back().payload.get())->value == 84, "typed reply preserved");
    Check(!provider->Respond(request, base) && !client->Cancel(request), "duplicate or late response/cancel ignored");
    autoReply = false;
    request = client->Request("compute", base, complete);
    Drain();
    Check(!observer->Respond(request, base) && !observer->Cancel(request), "only provider may respond and requester may cancel");
    Check(client->Cancel(request), "cancel pending request");
    Drain();
    Check(results.back().requestId == request && results.back().status == RequestStatus::Cancelled && cancellations == 1, "cancel terminal result and provider signal");
    Check(!provider->Respond(request, base), "cancelled request rejects late response");

    const auto beforeTimeout = results.size();
    request = client->Request("compute", base, complete, 15);
    Wait([&] { return results.size() > beforeTimeout; });
    Check(results.back().requestId == request && results.back().status == RequestStatus::TimedOut, "timeout terminal result");
    Check(!provider->Respond(request, base), "expired response ignored");
    Drain(); Check(cancellations == 2, "timeout notifies provider");
    request = client->Request("compute", base, complete, 10);
    QThread::msleep(15);
    Check(!provider->Respond(request, base), "deadline enforced even before timer event runs");
    Drain(); Check(results.back().status == RequestStatus::TimedOut, "late response becomes timeout");

    request = client->Request("compute", base, complete);
    provider.reset();
    Drain(); Check(results.back().requestId == request && results.back().status == RequestStatus::EndpointUnavailable, "endpoint invalidation fails queued request");
    auto replacement = loop.CreateEndpoint("provider", [](const EventDelivery&) {});
    Check(replacement && replacement->Provide("compute"), "name/capability released on invalidation");
    Check(!replacement->Respond(request, base), "new endpoint cannot answer stale request");
    const auto beforeDestroy = results.size();
    request = client->Request("compute", base, complete);
    client.reset(); Drain();
    Check(results.size() == beforeDestroy, "requester invalidation suppresses owned completion");

    auto cleanup = std::shared_ptr<EventEndpoint>(loop.CreateEndpoint("cleanup", [](const EventDelivery&) {}));
    auto doomed = loop.CreateEndpoint("doomed", [](const EventDelivery&) {});
    doomed->Request("compute", base, [cleanup](const RequestResult&) {});
    cleanup.reset();
    doomed.reset(); Drain();
    Check(loop.CreateEndpoint("cleanup", [](const EventDelivery&) {}) != nullptr,
          "retired completion captures can unregister another endpoint without deadlock");

    auto throwing = loop.CreateEndpoint("throwing", [](const EventDelivery& message) {
        if (message.kind == EventDelivery::Kind::Request) throw std::runtime_error("intentional handler failure");
    });
    throwing->Provide("throws");
    auto exceptionClient = loop.CreateEndpoint("exception-client", [](const EventDelivery&) {});
    exceptionClient->Request("throws", base, complete); Drain();
    Check(results.back().status == RequestStatus::HandlerFailed, "handler exceptions terminate request without wedging queue");

    EventEndpoint* self = nullptr;
    int selfCalls = 0;
    auto selfInvalidating = loop.CreateEndpoint("self-invalidating", [&](const EventDelivery&) { ++selfCalls; self->Invalidate(); });
    self = selfInvalidating.get(); self->Subscribe("self");
    exceptionClient->Notify("self", base); exceptionClient->Notify("self", base); Drain();
    Check(selfCalls == 1, "endpoint may invalidate itself safely during callback");

    auto inactive = loop.CreateEndpoint("inactive", [](const EventDelivery&) {}, false);
    Check(inactive->Provide("inactive-capability") && !inactive->Notify("changed", base), "inactive endpoints register but cannot send");
    auto workerClient = loop.CreateEndpoint("worker-client", [](const EventDelivery&) {});
    workerClient->Request("inactive-capability", base, complete); Drain();
    Check(results.back().status == RequestStatus::NoProvider, "inactive providers do not resolve");
    Check(inactive->Activate(), "activate staged endpoint");

    bool workerPost = false, workerNotify = false, workerRegistration = true;
    RequestId workerRequest = 0;
    std::thread worker([&] {
        workerPost = loop.Post({"worker"});
        workerNotify = workerClient->Notify("changed", base);
        workerRequest = workerClient->Request("missing", base, complete);
        workerRegistration = static_cast<bool>(loop.CreateEndpoint("wrong-thread", [](const EventDelivery&) {}));
        Check(!loop.Register(&legacy) && !observer->Subscribe("thread-topic"), "registration is loop-thread only");
    });
    worker.join(); Drain();
    Check(workerPost && workerNotify && workerRequest && !workerRegistration, "worker posting and explicit thread restrictions");
    Check(results.back().requestId == workerRequest && notificationsB == 2, "worker work dispatched in main queue");

    QStringList sequence;
    auto nested = loop.CreateEndpoint("nested", [&](const EventDelivery&) {
        sequence.append("notification");
        loop.Post({"nested-legacy"});
        QCoreApplication::processEvents();
        sequence.append("after-nested-pump");
    });
    nested->Subscribe("nested");
    legacy.callback = [&](const ProcessorEvent& event) { sequence.append(event.type); return false; };
    workerClient->Notify("nested", base);
    loop.Post({"already-queued"}); Drain();
    Check(sequence == QStringList{"notification", "after-nested-pump", "already-queued", "nested-legacy"}, "new and legacy events share nonrecursive FIFO");

    int destroyedCallback = 0;
    auto shortLivedLoop = std::unique_ptr<ProcessorEventLoop>(new ProcessorEventLoop);
    auto survivingEndpoint = shortLivedLoop->CreateEndpoint("survivor", [&](const EventDelivery&) { ++destroyedCallback; });
    survivingEndpoint->Subscribe("x"); survivingEndpoint->Notify("x", base);
    shortLivedLoop.reset(); Drain();
    Check(!survivingEndpoint->Notify("x", base) && destroyedCallback == 0, "endpoint safely outlives loop");
    {
        ProcessorEventLoop bounded;
        int delivered = 0, settled = 0;
        auto sink = bounded.CreateEndpoint("sink", [&](const EventDelivery& delivery) {
            if (delivery.kind == EventDelivery::Kind::Notification) ++delivered;
        });
        sink->Subscribe("flood"); sink->Provide("hold");
        auto producer = bounded.CreateEndpoint("producer", [](const EventDelivery&) {});
        for (size_t i = 0; i < ProcessorEventLoop::MaxQueuedDeliveries; ++i)
            Check(producer->Notify("flood", base), "admit notification within budget");
        Check(!producer->Notify("flood", base), "notification saturation explicitly rejects admission");
        Check(!producer->Request("hold", base, [](const RequestResult&) {}), "request saturation explicitly rejects admission");
        for (size_t i = 0; i < ProcessorEventLoop::MaxQueuedDeliveries + 10; ++i) QCoreApplication::processEvents();
        Check(delivered == int(ProcessorEventLoop::MaxQueuedDeliveries), "accepted notifications are not silently dropped");
        for (size_t i = 0; i < ProcessorEventLoop::MaxPendingRequests; ++i)
            Check(producer->Request("hold", base, [&](const RequestResult& result) {
                if (result.status == RequestStatus::EndpointUnavailable) ++settled;
            }) != 0, "admit pending request within budget");
        Check(!producer->Request("hold", base, [](const RequestResult&) {}), "pending request budget enforced");
        sink->Invalidate();
        for (size_t i = 0; i < ProcessorEventLoop::MaxQueuedDeliveries + 10; ++i) QCoreApplication::processEvents();
        Check(settled == int(ProcessorEventLoop::MaxPendingRequests), "terminal completions are reserved even under saturation");
    }
    loop.Unregister(&legacy);
    std::cout << "Plugin event contracts passed\n";
    return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
