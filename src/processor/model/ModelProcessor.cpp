#include "ModelProcessor.hpp"
#include "ModelRuntime.hpp"
#include <QJsonDocument>
#include <QTimer>

namespace {
const QString Operation = QStringLiteral("model.operation");
const QString Completed = QStringLiteral("model.completed");
}

ModelProcessor::ModelProcessor(ProcessorEventLoop& loop) : Processor(loop) {}
ModelProcessor::~ModelProcessor() { Stop(); }

bool ModelProcessor::Start(std::unique_ptr<ModelRenderTarget> target, const ModelRuntimeOptions& options, int width, int height)
{
    if (_worker || !target) return false;
    _metadata = {};
    _stoppedCleanly = true;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _state = {}; _frame.reset(); _error.clear();
    }
    _target = std::move(target);
    if (!_target->Prepare(&_thread)) { _target.reset(); return false; }
    _worker = new QObject;
    _worker->moveToThread(&_thread);
    connect(&_thread, &QThread::finished, _worker, &QObject::deleteLater);
    _thread.setObjectName(QStringLiteral("model-render"));
    _thread.start();
    bool initialized = false;
    QMetaObject::invokeMethod(_worker, [&] {
        _runtime.reset(new ModelRuntime);
        initialized = _target->Initialize() && _runtime->Initialize(options, width, height);
        if (initialized) {
            _metadata = _runtime->Metadata(); // 启动同步屏障；GUI 此时尚未派发事件。
            _state.width = width; _state.height = height;
            _state.app.modelIndex = _metadata.index;
            _state.modelToken = _metadata.hotkeys.value("model_token").toString().toStdString();
            _state.clip = {0, 0, width, height};
            auto* timer = new QTimer(_worker);
            timer->setTimerType(Qt::PreciseTimer);
            connect(timer, &QTimer::timeout, _worker, [this] {
                ModelRenderState state;
                { std::lock_guard<std::mutex> lock(_mutex); state = _state; }
                try {
                    auto frame = _runtime->Render(state, *_target);
                    std::lock_guard<std::mutex> lock(_mutex); _frame = std::move(frame);
                } catch (const std::exception& error) {
                    std::lock_guard<std::mutex> lock(_mutex); _error = QString::fromUtf8(error.what());
                    for (auto* timer : _worker->findChildren<QTimer*>()) timer->stop();
                }
            });
            timer->start(16);
        }
    }, Qt::BlockingQueuedConnection);
    _running = initialized;
    if (!initialized) Stop();
    return initialized;
}

bool ModelProcessor::Stop()
{
    _running = false;
    if (!_worker) return _stoppedCleanly;
    QMetaObject::invokeMethod(_worker, [this] {
        for (auto* timer : _worker->findChildren<QTimer*>()) timer->stop();
        _stoppedCleanly = _runtime->Shutdown(_target->MakeCurrent());
        _runtime.reset();
        _stoppedCleanly = _target->Shutdown() && _stoppedCleanly;
    }, Qt::BlockingQueuedConnection);
    _thread.quit(); _thread.wait();
    _worker = nullptr;
    _target.reset();
    return _stoppedCleanly;
}

bool ModelProcessor::Submit(const std::string& json, std::function<void(QJsonObject)> complete)
{
    if (!_running || !Error().isEmpty()) return false;
    return Send(Operation, this, {{"operation", QVariant::fromValue(std::make_shared<ModelOperation>(ModelOperation{json, std::move(complete)}))}});
}

bool ModelProcessor::HandleEvent(const ProcessorEvent& event)
{
    if (event.type != Operation && event.type != Completed) return false;
    auto operation = event.data.value("operation").value<ModelOperationPtr>();
    if (!operation && event.type == Operation) {
        IProcessor* requester = event.sender;
        operation = std::make_shared<ModelOperation>(ModelOperation{
            event.data.value("json").toString().toStdString(),
            [this, requester](QJsonObject reply) { Send(QStringLiteral("model.result"), requester, {{"reply", reply}}); }});
    }
    if (!operation) return true;
    if (event.type == Completed) {
        const int previous = _metadata.index;
        _metadata = event.data.value("metadata").value<ModelMetadata>();
        if (previous != _metadata.index && _modelChanged) _modelChanged();
        if (operation->complete) operation->complete(event.data.value("reply").toJsonObject());
    } else if (!_running || !Error().isEmpty()) {
        if (operation->complete) operation->complete({{"ok", false}, {"error", "model renderer stopped"}});
    } else {
        QMetaObject::invokeMethod(_worker, [this, operation] {
            auto reply = _runtime->Execute(operation->json);
            Send(Completed, this, {{"operation", QVariant::fromValue(operation)}, {"reply", reply},
                {"metadata", QVariant::fromValue(_runtime->Metadata())}});
        }, Qt::QueuedConnection);
    }
    return true;
}

void ModelProcessor::SetState(ModelRenderState state)
{ std::lock_guard<std::mutex> lock(_mutex); _state = std::move(state); }
std::shared_ptr<const RenderFrame> ModelProcessor::Frame() const
{ std::lock_guard<std::mutex> lock(_mutex); return _frame; }
QString ModelProcessor::Error() const
{ std::lock_guard<std::mutex> lock(_mutex); return _error; }
