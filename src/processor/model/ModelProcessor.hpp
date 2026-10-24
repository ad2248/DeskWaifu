#pragma once

#include "processor/Processor.hpp"
#include "RenderFrame.hpp"
#include "ModelRenderTarget.hpp"
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

// GUI-side operational adapter. Frames use a bounded latest-state/latest-frame
// mailbox, never per-frame ProcessorEvents or an unbounded Qt signal queue.
class ModelProcessor final : public Processor
{
public:
    explicit ModelProcessor(ProcessorEventLoop& loop);
    ~ModelProcessor() override;
    bool Start(std::unique_ptr<ModelRenderTarget> target, const ModelRuntimeOptions& options, int width, int height);
    bool Stop(); // Join before unregistering, then destroy the GUI-owned surface.
    bool HandleEvent(const ProcessorEvent& event) override;
    // Native events: model.operation {json: string} -> model.result {reply: object},
    // with result receiver = original sender. Submit is the legacy callback adapter.
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
};
