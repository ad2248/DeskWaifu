#include "event/ProcessorEventLoop.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <iostream>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ProcessorEventLoop loop;
    QEventLoop pump;
    bool flood = true, completedDuringFlood = false, admitted = true;
    int notifications = 0, completions = 0, cancellations = 0;
    RequestStatus status = RequestStatus::Replied;
    auto provider = loop.CreateEndpoint("provider", [&](const EventDelivery& delivery) {
        if (delivery.kind == EventDelivery::Kind::Cancellation) ++cancellations;
        // Intentionally never reply.
    });
    provider->Provide("hold");
    auto client = loop.CreateEndpoint("client", [](const EventDelivery&) {});
    EventEndpoint* repeater = nullptr;
    auto endpoint = loop.CreateEndpoint("repeater", [&](const EventDelivery&) {
        ++notifications;
        if (flood) admitted &= repeater->Notify("tick", std::make_shared<ProcessorEvent>());
    });
    repeater = endpoint.get(); repeater->Subscribe("tick");
    const auto request = client->Request("hold", std::make_shared<ProcessorEvent>(), [&](const RequestResult& result) {
        ++completions; status = result.status; completedDuringFlood = flood;
        if (cancellations == 1) pump.quit();
    }, 10);
    admitted &= request != 0 && repeater->Notify("tick", std::make_shared<ProcessorEvent>());
    QTimer::singleShot(150, &pump, [&] { flood = false; pump.quit(); });
    pump.exec();
    flood = false;
    for (int i = 0; i < 30; ++i) QCoreApplication::processEvents();
    if (!admitted || notifications < 2 || !completedDuringFlood || completions != 1 || cancellations != 1 ||
        status != RequestStatus::TimedOut || provider->Respond(request, std::make_shared<ProcessorEvent>())) {
        std::cerr << "Timeout must settle once and cancel the provider while notifications continue: "
                  << "notifications=" << notifications << " completions=" << completions
                  << " cancellations=" << cancellations << " during_flood=" << completedDuringFlood << '\n';
        return 1;
    }
    return 0;
}
