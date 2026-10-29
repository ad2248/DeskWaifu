#include "plugin/PluginApi.hpp"
#include "plugin/PluginContext.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>

namespace {
class EarlyDesktopClient final : public IProcessorPlugin
{
public:
    bool Initialize(PluginContext& context, QString*) override
    {
        _endpoint = context.CreateEndpoint("early", [this](const EventDelivery& delivery) {
            if (delivery.kind != EventDelivery::Kind::Request || !delivery.payload) return;
            if (delivery.topic == "fixture.desktop.early-results/v1") {
                _endpoint->Respond(delivery.requestId, std::make_shared<ProcessorEvent>("fixture.results", nullptr, nullptr,
                    QVariantMap{{"records", _records}}));
            } else Probe(delivery.payload->data.value("phase").toString(), [this, id = delivery.requestId] {
                _endpoint->Respond(id, std::make_shared<ProcessorEvent>("fixture.probed"));
            });
        });
        return _endpoint && _endpoint->Provide("fixture.desktop.early-probe/v1") &&
            _endpoint->Provide("fixture.desktop.early-results/v1");
    }
    bool Start(QString*) override
    {
        // This module is installed as a/a.so, so the real desktop Start has not
        // run yet. Both capabilities are already published by Initialize.
        bool done = false;
        Probe("before_start", [&] { done = true; });
        QElapsedTimer timer; timer.start();
        while (!done && timer.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(1); }
        return done;
    }
    void Stop() noexcept override {}
private:
    void Probe(const QString& phase, std::function<void()> finished)
    {
        auto remaining = std::make_shared<int>(2);
        for (const auto& command : QStringList{"get_status", "set_model"}) {
            const auto json = QJsonDocument(QJsonObject{{"command", command}, {"tag", phase + '-' + command}})
                .toJson(QJsonDocument::Compact);
            _endpoint->Request("command." + command + "/v1",
                std::make_shared<ProcessorEvent>("command", nullptr, nullptr, QVariantMap{{"json", QString::fromUtf8(json)}}),
                [this, phase, command, remaining, finished](const RequestResult& result) {
                    _records.append(QJsonObject{{"phase", phase}, {"command", command},
                        {"status", RequestStatusName(result.status)}, {"response", result.payload
                            ? QJsonDocument::fromJson(result.payload->data.value("json").toString().toUtf8()).object() : QJsonObject{}}});
                    if (--*remaining == 0) finished();
                }, 1000);
        }
    }
    EventEndpoint* _endpoint = nullptr;
    QJsonArray _records;
};
}
DESKWAIFU_EXPORT_PLUGIN(EarlyDesktopClient)
