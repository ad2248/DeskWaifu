#include "ModelProcessor.hpp"
#include "ModelRuntime.hpp"
#include <QJsonDocument>
#include <QTimer>

namespace {
const QString Operation = QStringLiteral("model.operation");
const QString Completed = QStringLiteral("model.state");
}

ModelProcessor::ModelProcessor(ProcessorEventLoop& loop, std::shared_ptr<ModelExchange> exchange)
    : Processor(loop), _exchange(std::move(exchange)) {}
ModelProcessor::~ModelProcessor() { Stop(); }

bool ModelProcessor::Start(std::unique_ptr<ModelRenderTarget> target, const ModelRuntimeOptions& options, int width, int height)
{
    if (_worker || !target) return false;
    _hits = std::make_shared<ModelHitMailbox>();
    _metadata = {};
    _stoppedCleanly = true;
    {
        std::lock_guard<std::mutex> lock(_exchange->mutex);
        _exchange->frame.reset(); _exchange->error.clear();
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
            {
                std::lock_guard<std::mutex> lock(_exchange->mutex);
                _exchange->state.width = width; _exchange->state.height = height;
                _exchange->state.app.modelIndex = _metadata.index;
                _exchange->state.modelToken = _metadata.hotkeys.value("model_token").toString().toStdString();
                _exchange->state.clip = {0, 0, width, height};
            }
            auto* timer = new QTimer(_worker);
            timer->setTimerType(Qt::PreciseTimer);
            connect(timer, &QTimer::timeout, _worker, [this] {
                ModelRenderState state;
                { std::lock_guard<std::mutex> lock(_exchange->mutex); state = _exchange->state; }
                try {
                    auto frame = _runtime->Render(state, *_target);
                    { std::lock_guard<std::mutex> lock(_hits->mutex); _hits->snapshot = frame->hit; }
                    std::lock_guard<std::mutex> lock(_exchange->mutex); _exchange->frame = std::move(frame);
                } catch (const std::exception& error) {
                    std::lock_guard<std::mutex> lock(_exchange->mutex); _exchange->error = QString::fromUtf8(error.what());
                    { std::lock_guard<std::mutex> hitLock(_hits->mutex); _hits->active = false; }
                    for (auto* timer : _worker->findChildren<QTimer*>()) timer->stop();
                }
            });
            timer->start(16);
        }
    }, Qt::BlockingQueuedConnection);
    _running = initialized;
    if (initialized) Send("model.state", nullptr, {{"metadata", QVariant::fromValue(_metadata)}});
    if (!initialized) Stop();
    return initialized;
}

bool ModelProcessor::Stop()
{
    _running = false;
    if (_hits) { std::lock_guard<std::mutex> lock(_hits->mutex); _hits->active = false; _hits->snapshot.reset(); }
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

bool ModelProcessor::HandleEvent(const ProcessorEvent& event)
{
    if (event.type == "model.hit_test.request") {
        ModelHitTest callback;
        if (_running && _hits) callback = _hits->Callback(_hits);
        Send("model.hit_test.ready", event.sender, {{"callback", QVariant::fromValue(callback)}});
        return true;
    }
    if (event.type != Operation && event.type != Completed) return false;
    if (event.type == Completed) {
        if (event.sender != this) return false;
        _metadata = event.data.value("metadata").value<ModelMetadata>();
        return false;
    } else {
        bool failed;
        { std::lock_guard<std::mutex> lock(_exchange->mutex); failed = !_exchange->error.isEmpty(); }
        if (!_running || failed) {
            Send("model.result", event.sender, {{"reply", QJsonObject{{"ok", false}, {"error", "model renderer stopped"}}}});
            return true;
        }
        const auto json = event.data.value("json").toString();
        IProcessor* requester = event.sender;
        QMetaObject::invokeMethod(_worker, [this, json, requester] {
            auto reply = _runtime->Execute(json.toStdString());
            const auto command = QJsonDocument::fromJson(json.toUtf8()).object().value("command").toString();
            if (reply.value("ok").toBool() && (command == "set_model" || command == "next_model" || command == "prev_model" || command == "switch_skin")) {
                std::lock_guard<std::mutex> lock(_hits->mutex); _hits->snapshot.reset();
            }
            Send(Completed, nullptr, {{"metadata", QVariant::fromValue(_runtime->Metadata())}});
            Send("model.result", requester, {{"reply", reply}});
        }, Qt::QueuedConnection);
    }
    return true;
}
