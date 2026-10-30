#include "OcrClient.hpp"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <cmath>
#include <utility>

namespace {
constexpr int MaxReplyBytes = 4 * 1024 * 1024;
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

QString OcrClient::DefaultExecutable()
{
    return qEnvironmentVariable("DESKWAIFU_OCR_HELPER", QStringLiteral(DESKWAIFU_OCR_HELPER_PATH));
}

OcrClient::OcrClient(QString executable, int timeoutMs, int idleMs) : _executable(std::move(executable))
{
    _deadline.setSingleShot(true);
    _deadline.setInterval(timeoutMs);
    _idle.setSingleShot(true);
    _idle.setInterval(idleMs);
    connect(&_deadline, &QTimer::timeout, this, [this] { Fail("OCR initialization or request timed out", "timeout"); });
    connect(&_idle, &QTimer::timeout, this, [this] { _ready = false; _process.kill(); });
    connect(&_process, &QProcess::readyReadStandardOutput, this, [this] { ReadOutput(); });
    // 不保留无界诊断输出，也不将图像内容写入应用日志。
    connect(&_process, &QProcess::readyReadStandardError, this, [this] { _process.readAllStandardError(); });
    connect(&_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) Fail("OCR helper failed to start: " + _process.errorString());
    });
    connect(&_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int exitCode, QProcess::ExitStatus exitStatus) {
        const bool initializing = _checking;
        const bool unexpectedExit = _ready && (exitCode != 0 || exitStatus == QProcess::CrashExit);
        _ready = false;
        _buffer.clear();
        _idle.stop();
        if (Pending() || initializing || unexpectedExit) Fail("OCR helper exited before completing initialization or request");
        emit WorkerStopped();
    });
}

OcrClient::~OcrClient()
{
    _process.disconnect(this);
    if (_process.state() != QProcess::NotRunning) {
        _process.kill();
        _process.waitForFinished(1000);
    }
}

QJsonObject OcrClient::Status() const
{
    const QFileInfo helper(_executable);
    return {{"ok", true}, {"available", _configured ? _availability == "available" : helper.isFile() && helper.isExecutable()},
        {"availability_state", _availability}, {"error_code", _errorCode}, {"error", _error},
        {"detection_model_dir", _detectionDir}, {"recognition_model_dir", _recognitionDir},
        {"state", _state}, {"job_id", _id}, {"running", _process.state() != QProcess::NotRunning}};
}

void OcrClient::ConfigureModels(const QJsonObject& config, const QString& configError)
{
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
    const QFileInfo helper(_executable);
    if (!helper.isFile() || !helper.isExecutable()) {
        Fail("OCR feature package is not installed or executable", "helper_missing"); return;
    }
    _availability = "checking";
    _checking = true;
    _deadline.start();
    StartWorker();
}

void OcrClient::StartWorker()
{
    QStringList arguments;
    if (_configured) {
        _availability = "checking";
        arguments << "--detection-model-dir" << _detectionDir << "--recognition-model-dir" << _recognitionDir;
    }
    arguments << "--serve";
    _buffer.clear();
    _process.start(_executable, arguments);
}

QJsonObject OcrClient::Submit(const QString& image)
{
    if (Pending()) return Error("OCR is busy");
    const bool recoverable = _validated && (_errorCode == "helper_error" || _errorCode == "timeout");
    if (_configured && _availability != "available" && !recoverable)
        return {{"ok", false}, {"error", _availability == "checking" ? "OCR is initializing; retry shortly" : _error},
            {"error_code", _availability == "checking" ? "initializing" : _errorCode}};
    if (_process.state() != QProcess::NotRunning && !_ready) return Error("OCR helper is stopping; retry shortly");
    if (!Status().value("available").toBool() && !recoverable) return Error("OCR feature package is not installed");
    const QFileInfo file(image);
    if (!file.isAbsolute() || !file.isFile() || !file.isReadable()) return Error("image must be an absolute readable file path");
    _idle.stop();
    _id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    _image = file.canonicalFilePath();
    _state = "pending";
    _reply = {};
    _deadline.start();
    if (_ready) SendRequest();
    else {
        StartWorker();
    }
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
    _ready = false;
    _checking = false;
    if (_configured && code == "cancelled" && _validated) {
        _availability = "available";
        _error.clear(); _errorCode.clear();
    }
    if (_configured && code != "cancelled") {
        _availability = "unavailable";
        _error = error;
        _errorCode = code;
    }
    _buffer.clear();
    const bool notify = Pending();
    if (notify) {
        _state = "failed";
        _reply = Error(error);
    }
    if (_process.state() != QProcess::NotRunning) _process.kill();
    if (notify) {
        const auto request = _id;
        const auto response = _reply;
        QTimer::singleShot(0, this, [this, request, response] { emit Finished(request, response); });
    }
}

void OcrClient::SendRequest()
{
    const auto data = QJsonDocument(QJsonObject{{"protocol", 1}, {"id", _id}, {"image", _image}}).toJson(QJsonDocument::Compact) + '\n';
    if (_process.write(data) != data.size()) Fail("failed to write OCR request");
}

void OcrClient::ReadOutput()
{
    // Cancel/idle shutdown may leave a ready event or completed reply buffered.
    // It belongs to the retired worker, not a new initialization handshake.
    if (!_ready && !_checking && !Pending()) {
        _process.readAllStandardOutput();
        return;
    }
    _buffer += _process.readAllStandardOutput();
    if (_buffer.size() > MaxReplyBytes) { Fail("OCR reply exceeded size limit"); return; }
    while (_buffer.contains('\n')) {
        const auto end = _buffer.indexOf('\n');
        const auto line = _buffer.left(end);
        _buffer.remove(0, end + 1);
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(line, &parse);
        const auto reply = document.object();
        if (parse.error != QJsonParseError::NoError || !document.isObject() || reply.value("protocol") != QJsonValue(1)) {
            Fail("invalid OCR protocol reply"); return;
        }
        if (!_ready) {
            if (reply.value("event") == QJsonValue("error") && reply.value("error").isString()) {
                Fail("OCR initialization failed: " + reply.value("error").toString(),
                    reply.value("code").toString("model_load_failed")); return;
            }
            if ((!Pending() && !_checking) || reply.value("event") != QJsonValue("ready")) { Fail("OCR handshake failed"); return; }
            _ready = true;
            _validated = true;
            _checking = false;
            _availability = "available";
            _error.clear(); _errorCode.clear();
            if (Pending()) SendRequest();
            else { _deadline.stop(); _idle.start(); }
        } else {
            if (!Pending() || reply.value("id") != QJsonValue(_id) || !reply.value("ok").isBool() ||
                (reply.value("ok").toBool() ? !ValidResult(reply.value("result").toObject()) : !reply.value("error").isString())) {
                Fail("invalid OCR response or request identifier"); return;
            }
            _deadline.stop();
            _reply = reply;
            _state = reply.value("ok").toBool() ? "completed" : "failed";
            _idle.start();
            const auto request = _id;
            const auto response = _reply;
            QTimer::singleShot(0, this, [this, request, response] { emit Finished(request, response); });
        }
    }
}
