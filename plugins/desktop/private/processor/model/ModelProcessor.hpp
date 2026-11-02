#pragma once

#include "processor/Processor.hpp"
#include "RenderFrame.hpp"
#include "ModelExchange.hpp"
#include "render/ModelRenderTarget.hpp"
#include <QThread>

class ModelRuntime;

// GUI 侧的操作适配器。帧使用有界的最新状态和最新帧交换槽，
// 不使用逐帧 ProcessorEvent 或无界 Qt 信号队列。
class ModelProcessor final : public Processor
{
public:
    explicit ModelProcessor(ProcessorEventLoop& loop, std::shared_ptr<ModelExchange> exchange);
    ~ModelProcessor() override;
    bool Start(std::unique_ptr<ModelRenderTarget> target, const ModelRuntimeOptions& options, int width, int height);
    void Stop() noexcept override { Shutdown(); }
    bool Shutdown() noexcept; // 等待线程结束，报告 GPU 上下文收尾结果。
    bool HandleEvent(const ProcessorEvent& event) override;
private:
    friend struct ProcessorTestAccess;
    QThread _thread;
    QObject* _worker = nullptr;
    std::unique_ptr<ModelRenderTarget> _target;
    std::unique_ptr<ModelRuntime> _runtime;
    ModelMetadata _metadata;
    std::shared_ptr<ModelExchange> _exchange;
    bool _running = false, _stoppedCleanly = true;
    std::shared_ptr<ModelHitMailbox> _hits;
};
