#include "OcrClient.hpp"
#include "OcrBackend.hpp"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QPluginLoader>
#include <QUuid>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {
QJsonObject Error(const QString& error) { return {{"ok", false}, {"error", error}}; }

bool ValidResult(const QJsonObject& result)
{
    const auto width = result.value("width").toInt();
    const auto height = result.value("height").toInt();
    if (width < 1 || width > 4096 || height < 1 || height > 4096 || !result.value("lines").isArray()) return false;
    for (const auto& value : result.value("lines").toArray()) {
        const auto line = value.toObject();
        const auto score = line.value("confidence");
        const auto polygon = line.value("polygon").toArray();
        if (!line.value("text").isString() || !score.isDouble() ||
            !std::isfinite(score.toDouble()) || score.toDouble() < 0 || score.toDouble() > 1 || polygon.size() != 4) return false;
        for (const auto& point : polygon) {
            const auto xy = point.toArray();
            if (xy.size() != 2) return false;
            for (int axis = 0; axis < 2; ++axis)
                if (!xy[axis].isDouble() || !std::isfinite(xy[axis].toDouble()) ||
                    xy[axis].toDouble() < 0 || xy[axis].toDouble() > (axis == 0 ? width : height)) return false;
        }
    }
    return true;
}
}

// Accessed only on the dedicated thread, including module destruction/unloading.
class OcrClient::Worker : public QObject
{
public:
    Worker() : loader(this) {}
    QPluginLoader loader;
    std::unique_ptr<OcrBackend> backend;

    QJsonObject Load(const QString& module, const QString& detection, const QString& recognition)
    {
        loader.setFileName(module);
        loader.setLoadHints(QLibrary::ResolveAllSymbolsHint);
        if (loader.metaData().value("IID") != QJsonValue(DeskWaifuOcrBackend_iid))
            return {{"ok", false}, {"code", "module_incompatible"}, {"error", "OCR module is invalid or incompatible; reinstall the matching OCR package: " + loader.errorString()}};
        auto* instance = loader.instance();
        auto* factory = qobject_cast<OcrBackendFactory*>(instance);
        if (!factory)
            return {{"ok", false}, {"code", "module_load_failed"}, {"error", "Cannot load OCR module; check its runtime dependencies: " + loader.errorString()}};
        backend.reset(factory->Create());
        if (!backend) throw std::runtime_error("OCR module could not create an engine");
        return backend->Initialize(detection, recognition);
    }

    void Release()
    {
        if (backend) backend->Release();
        backend.reset();
        loader.unload();
    }
};

QString OcrClient::DefaultModule()
{
    return qEnvironmentVariable("DESKWAIFU_OCR_MODULE", QStringLiteral(DESKWAIFU_OCR_MODULE_PATH));
}

OcrClient::OcrClient(QString module, int timeoutMs, int idleMs)
    : _worker(new Worker), _module(std::move(module))
{
    _worker->moveToThread(&_thread);
    connect(&_thread, &QThread::finished, _worker, &QObject::deleteLater);
    _thread.start();
    _deadline.setSingleShot(true);
    _deadline.setInterval(timeoutMs);
    _idle.setSingleShot(true);
    _idle.setInterval(idleMs);
    connect(&_deadline, &QTimer::timeout, this, [this] { Fail("OCR initialization or request timed out", "timeout"); });
    connect(&_idle, &QTimer::timeout, this, [this] { ReleaseWorker(); });
}

OcrClient::~OcrClient()
{
    // Never terminate a native inference thread. Keep borrowed files and code alive until it returns.
    QMetaObject::invokeMethod(_worker, [worker = _worker] { worker->Release(); }, Qt::BlockingQueuedConnection);
    _thread.quit();
    _thread.wait();
}

QJsonObject OcrClient::Status() const
{
    return {{"ok", true}, {"available", _availability == "available"},
        {"availability_state", _availability}, {"error_code", _errorCode}, {"error", _error},
        {"detection_model_dir", _detectionDir}, {"recognition_model_dir", _recognitionDir},
        {"state", _state}, {"job_id", _id}, {"running", _loaded || _working},
        {"busy", _working}, {"retiring", _retiring}, {"execution_mode", "in_process"}};
}

void OcrClient::ConfigureModels(const QJsonObject& config, const QString& configError)
{
    if (_working || _loaded) return;
    _configured = true;
    _detectionDir = "/usr/share/deskwaifu/ocr/models/PP-OCRv6_small_det";
    _recognitionDir = "/usr/share/deskwaifu/ocr/models/PP-OCRv6_small_rec";
    if (!configError.isEmpty()) { Fail(configError, "invalid_config"); return; }
    for (const auto& item : {qMakePair(QStringLiteral("ocr_detection_model_dir"), &_detectionDir),
                            qMakePair(QStringLiteral("ocr_recognition_model_dir"), &_recognitionDir)}) {
        if (config.contains(item.first)) {
            const auto value = config.value(item.first);
            if (!value.isString() || value.toString().contains(QChar::Null) || !QDir::isAbsolutePath(value.toString())) {
                Fail("OCR configuration requires an absolute directory: " + item.first, "invalid_config"); return;
            }
            *item.second = value.toString();
        }
    }
    if (!QFileInfo(_module).isFile()) {
        Fail("OCR feature module is not installed; install the matching OCR package", "module_missing"); return;
    }
    _deadline.start();
    StartWorker();
}

void OcrClient::StartWorker()
{
    _working = true;
    _availability = "checking";
    QMetaObject::invokeMethod(_worker, [this, worker = _worker, module = _module, detection = _detectionDir, recognition = _recognitionDir] {
        QJsonObject reply;
        try { reply = worker->Load(module, detection, recognition); }
        catch (const std::exception& error) { reply = {{"ok", false}, {"code", "model_load_failed"}, {"error", QString::fromUtf8(error.what())}}; }
        QMetaObject::invokeMethod(this, [this, reply] {
            _working = false;
            _loaded = true; // The module must be released even if initialization failed.
            if (_retiring) { ReleaseWorker(); return; }
            if (!reply.value("ok").toBool()) {
                Fail(reply.value("error").toString(), reply.value("code").toString("model_load_failed")); return;
            }
            _validated = true;
            _availability = "available";
            _error.clear(); _errorCode.clear();
            if (Pending()) SendRequest();
            else { _deadline.stop(); _idle.start(); }
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

QJsonObject OcrClient::Submit(const QString& image)
{
    if (Pending() || _working || _retiring) return Error("OCR is busy");
    const bool recoverable = _validated && (_errorCode == "backend_error" || _errorCode == "timeout");
    if (_configured && _availability != "available" && !recoverable)
        return {{"ok", false}, {"error", _availability == "checking" ? "OCR is initializing; retry shortly" : _error},
            {"error_code", _availability == "checking" ? "initializing" : _errorCode}};
    if (!QFileInfo(_module).isFile()) return Error("OCR feature module is not installed");
    const QFileInfo file(image);
    if (!file.isAbsolute() || !file.isFile() || !file.isReadable()) return Error("image must be an absolute readable file path");
    _idle.stop();
    _id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    _image = file.canonicalFilePath();
    _state = "pending";
    _reply = {};
    _deadline.start();
    if (_loaded) SendRequest();
    else StartWorker();
    return {{"ok", true}, {"job_id", _id}, {"state", "pending"}};
}

QJsonObject OcrClient::Result(const QString& id) const
{
    if (id.isEmpty() || id != _id) return Error("unknown OCR job; only the most recent job is retained");
    QJsonObject result{{"ok", true}, {"job_id", _id}, {"state", _state}};
    if (!Pending()) result.insert("response", _reply);
    return result;
}

QJsonObject OcrClient::Cancel(const QString& id)
{
    if (id.isEmpty() || id != _id || !Pending()) return Error("no matching pending OCR job");
    Fail("OCR request cancelled", "cancelled");
    return {{"ok", true}, {"job_id", _id}};
}

void OcrClient::Fail(const QString& error, const QString& code)
{
    _deadline.stop();
    _idle.stop();
    if (code != "cancelled") {
        _availability = "unavailable";
        _error = error; _errorCode = code;
    } else if (_validated) {
        _availability = "available";
        _error.clear(); _errorCode.clear();
    }
    if (Pending()) {
        _state = "failed";
        _reply = Error(error);
        const auto request = _id;
        const auto response = _reply;
        QTimer::singleShot(0, this, [this, request, response] { emit Finished(request, response); });
    }
    if (_working) _retiring = true;
    else if (_loaded) ReleaseWorker();
}

void OcrClient::ReleaseWorker()
{
    _working = true;
    _retiring = true;
    QMetaObject::invokeMethod(_worker, [this, worker = _worker] {
        worker->Release();
        QMetaObject::invokeMethod(this, [this] {
            _loaded = false;
            _working = false;
            _retiring = false;
            emit WorkerStopped();
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void OcrClient::SendRequest()
{
    _working = true;
    QMetaObject::invokeMethod(_worker, [this, worker = _worker, image = _image] {
        QJsonObject reply;
        try { reply = worker->backend->Recognize(image); }
        catch (const std::exception& error) { reply = Error(QString::fromUtf8(error.what())); }
        QMetaObject::invokeMethod(this, [this, reply] {
            _working = false;
            if (_retiring) { ReleaseWorker(); return; }
            if (!reply.value("ok").isBool() ||
                (reply.value("ok").toBool() ? !ValidResult(reply.value("result").toObject()) : !reply.value("error").isString())) {
                Fail("Invalid OCR module response"); return;
            }
            _deadline.stop();
            _reply = reply;
            _reply.insert("protocol", 1);
            _reply.insert("id", _id);
            _state = reply.value("ok").toBool() ? "completed" : "failed";
            _idle.start();
            const auto request = _id;
            const auto response = _reply;
            QTimer::singleShot(0, this, [this, request, response] { emit Finished(request, response); });
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}
