#include "plugin/PluginApi.hpp"
#include "plugin/PluginContext.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QThread>

namespace {
class EarlyPerceptionClient final : public IProcessorPlugin
{
public:
    bool Initialize(PluginContext& context, QString*) override
    {
        _context = &context;
        _endpoint = context.CreateEndpoint("early", [](const EventDelivery&) {});
        return _endpoint != nullptr;
    }
    bool Start(QString* error) override
    {
        const auto request = std::make_shared<ProcessorEvent>("command.invoke", nullptr, nullptr,
            QVariantMap{{"json", QStringLiteral("{\"command\":\"get_ocr_status\"}")}});
        if (!_endpoint->Request("command.get_ocr_status/v1", request, [this](const RequestResult& result) {
            _result = {{"completed", true}, {"status", RequestStatusName(result.status)},
                {"response", result.payload ? QJsonDocument::fromJson(result.payload->data.value("json").toString().toUtf8()).object() : QJsonObject{}}};
        })) return false;
        // This module sorts before perception. Pump real public deliveries while
        // its Start is still pending, just as an earlier event-pumping plugin can.
        QElapsedTimer timer; timer.start();
        while (!_result.value("completed").toBool() && timer.elapsed() < 250) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        return _context->WriteConfig("early.json", _result, error);
    }
    void Stop() noexcept override {}
private:
    PluginContext* _context = nullptr;
    EventEndpoint* _endpoint = nullptr;
    QJsonObject _result;
};
}
DESKWAIFU_EXPORT_PLUGIN(EarlyPerceptionClient)
