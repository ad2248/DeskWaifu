#pragma once

#include "processor/Processor.hpp"
#include "contracts/RenderFrame.hpp"
#include "render/ModelRenderTarget.hpp"
#include <QThread>
#include <functional>
#include <mutex>

class ModelRuntime;

struct ModelOperation
{
    std::string json;
    std::function<void(QJsonObject)> complete;
};
using ModelOperationPtr = std::shared_ptr<ModelOperation>;
Q_DECLARE_METATYPE(ModelOperationPtr)

// GUI 侧的操作适配器。帧使用有界的最新状态和最新帧交换槽，
// 不使用逐帧 ProcessorEvent 或无界 Qt 信号队列。
class ModelProcessor final : public Processor
{
public:
    explicit ModelProcessor(ProcessorEventLoop& loop);
    ~ModelProcessor() override;
    bool Start(std::unique_ptr<ModelRenderTarget> target, const ModelRuntimeOptions& options, int width, int height);
    bool Stop(); // 注销前等待线程结束，然后销毁 GUI 持有的表面。
    bool HandleEvent(const ProcessorEvent& event) override;
    // 原生事件：model.operation {json: string} -> model.result {reply: object}，
    // 结果接收者为原发送者。Submit 用于适配旧式回调。
    bool Submit(const std::string& json, std::function<void(QJsonObject)> complete = {});
    void SetState(ModelRenderState state);
    std::shared_ptr<const RenderFrame> Frame() const;
    const ModelMetadata& Metadata() const { return _metadata; }
    QString Error() const;
    void SetModelChanged(std::function<void()> changed) { _modelChanged = std::move(changed); }
private:
    QThread _thread;
    QObject* _worker = nullptr;
    std::unique_ptr<ModelRenderTarget> _target;
    std::unique_ptr<ModelRuntime> _runtime;
    ModelMetadata _metadata;
    mutable std::mutex _mutex;
    ModelRenderState _state;
    std::shared_ptr<const RenderFrame> _frame;
    QString _error;
    std::function<void()> _modelChanged;
    bool _running = false, _stoppedCleanly = true;
    std::shared_ptr<ModelHitMailbox> _hits;
};
