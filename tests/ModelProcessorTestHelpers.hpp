#pragma once
#include "processor/model/ModelProcessor.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QJsonDocument>

struct ProcessorTestAccess
{
    static ProcessorEventLoop& Loop(ModelProcessor& processor) { return processor._loop; }
    static const ModelMetadata& Metadata(ModelProcessor& processor) { return processor._metadata; }
    static std::shared_ptr<ModelExchange> Exchange(ModelProcessor& processor) { return processor._exchange; }
    static ModelProcessor& Models(class LAppDelegate& app);
    static class InteractionContext& Context(class LAppDelegate& app);
    static class InteractionProcessor& Interaction(class LAppDelegate& app);
};

inline const ModelMetadata& ModelMetadataOf(ModelProcessor& processor) { return ProcessorTestAccess::Metadata(processor); }
inline std::shared_ptr<const RenderFrame> ModelFrameOf(ModelProcessor& processor)
{
    const auto slot = ProcessorTestAccess::Exchange(processor);
    std::lock_guard<std::mutex> lock(slot->mutex); return slot->frame;
}
inline void SetModelState(ModelProcessor& processor, const ModelRenderState& state)
{
    const auto slot = ProcessorTestAccess::Exchange(processor);
    std::lock_guard<std::mutex> lock(slot->mutex); slot->state = state;
}
inline QString ModelErrorOf(ModelProcessor& processor)
{
    const auto slot = ProcessorTestAccess::Exchange(processor);
    std::lock_guard<std::mutex> lock(slot->mutex); return slot->error;
}

// 测试只在平台边界注入依赖；模型业务和实际完成通知走正式事件路径。
inline QJsonObject AwaitModel(ModelProcessor& processor, QJsonObject operation = {{"command", "reset_drag"}})
{
    class Receiver final : public Processor
    {
    public:
        using Processor::Processor;
        bool done = false;
        QJsonObject result;
        bool HandleEvent(const ProcessorEvent& event) override
        {
            if (event.type != "model.result") return false;
            result = event.data.value("reply").toJsonObject(); done = true; return true;
        }
    } receiver(ProcessorTestAccess::Loop(processor));
    if (!ProcessorTestAccess::Loop(processor).Post({"model.operation", &receiver, &processor,
        {{"json", QString::fromUtf8(QJsonDocument(operation).toJson(QJsonDocument::Compact))}}})) return {};
    QElapsedTimer timeout; timeout.start();
    while (!receiver.done && timeout.elapsed() < 5000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    if (!receiver.done) qFatal("Timed out waiting for model operation");
    return receiver.result;
}
