#include "VoiceInputController.hpp"
#include "asr/NativeAsrClient.hpp"
#include "audio/AudioCapture.hpp"
#include "contracts/ClipboardEvents.hpp"
#include "contracts/VoiceInputEvents.hpp"
#include <QPointer>

VoiceInputController::VoiceInputController(EventEndpoint& endpoint, VoiceInputSettings& settings, AudioCapture& capture, NativeAsrClient& asr, QObject* parent)
    : QObject(parent)
    , _endpoint(endpoint)
    , _settings(settings)
    , _capture(capture)
    , _asr(asr)
{
    _recordingLimit.setSingleShot(true);
    _progress.setInterval(1000);
    connect(&_recordingLimit, &QTimer::timeout, this, [this] { StopRecording(QString::number(_session)); });
    connect(&_progress, &QTimer::timeout, this, [this] { Publish(); });
    connect(&_capture, &AudioCapture::Failed, this, [this](quint64 id, const QString& code, const QString& error) {
        if (_phase == "starting" || _phase == "recording" || _phase == "stopping") {
            Fail(id, code, error);
        }
    });
    connect(&_asr, &NativeAsrClient::Failed, this, [this](quint64 id, const QString& code, const QString& error) {
        if (_phase == "transcribing") {
            Fail(id, code, error);
        }
    });
    connect(&_asr, &NativeAsrClient::Recognized, this, &VoiceInputController::OnRecognized);
}
VoiceInputController::~VoiceInputController() { Shutdown(); }

bool VoiceInputController::Move(Event event)
{
    State next;
    if (!VoiceInputState::Transition(_state, event, next)) {
        return false;
    }
    _state = next;
    return true;
}
bool VoiceInputController::IsCurrent(quint64 session) const { return !_shutdown && _state != State::Idle && session == _session; }

QJsonObject VoiceInputController::Status() const
{
    return { { "ok", true }, { "state", VoiceInputState::Name(_state) }, { "phase", _phase }, { "session_id", QString::number(_session) },
        { "elapsed_ms", _state == State::Recording && _elapsed.isValid() ? _elapsed.elapsed() : _recordedMs }, { "device_id", _snapshot.deviceId }, { "last_outcome", _outcome },
        { "error_code", _code }, { "error", _error }, { "copy_state", _copyState }, { "source", _recognitionRequest ? "request" : "recording" } };
}
QJsonObject VoiceInputController::Reply(bool ok, const QString& code, const QString& error) const
{
    auto reply = Status();
    reply.insert("ok", ok);
    if (!ok) {
        reply.insert("error_code", code);
        reply.insert("error", error);
    }
    return reply;
}
QJsonObject VoiceInputController::Result(const QString& session) const
{
    if (!_session || session != QString::number(_session)) {
        return Reply(false, "stale_session", "会话已过期");
    }
    if (_state != State::Idle) {
        return Reply(false, "busy", "语音任务尚未完成");
    }
    auto reply = Status();
    reply.insert("text", _text);
    return reply;
}
bool VoiceInputController::Prepare(QString& error)
{
    if (!_settings.Refresh(error) || !_settings.Configured()) {
        error = QStringLiteral("请先在控制面板配置 ASR 端点和 API_KEY");
        return false;
    }
    _snapshot = _settings.Snapshot();
    _session = ++_nextSession;
    _text.clear();
    _outcome.clear();
    _code.clear();
    _error.clear();
    _copyState = "not_requested";
    _recordedMs = 0;
    _elapsed.invalidate();
    return true;
}
QJsonObject VoiceInputController::StartRecording()
{
    if (_shutdown) {
        return Reply(false, "unavailable", "语音输入服务已停止");
    }
    if (_state != State::Idle) {
        return Reply(false, "busy", "语音任务进行中");
    }
    QString error;
    if (!Prepare(error)) {
        return Reply(false, "not_configured", error);
    }
    Move(Event::ToggleRecording);
    _phase = "starting";
    const auto session = _session;
    const bool started = _capture.Start(session, _snapshot.deviceId, _snapshot.maxRecordingSeconds, error);
    if (!IsCurrent(session)) {
        return Reply(false, _code, _error);
    }
    if (!started) {
        Fail(session, "capture_failed", "无法启动所选音频设备");
        return Reply(false, _code, _error);
    }
    _phase = "recording";
    _elapsed.start();
    _recordingLimit.start(_snapshot.maxRecordingSeconds * 1000);
    _progress.start();
    Publish();
    return Reply(true);
}
QJsonObject VoiceInputController::StopRecording(const QString& session)
{
    if (_shutdown) {
        return Reply(false, "unavailable", "语音输入服务已停止");
    }
    if (!_session || session != QString::number(_session)) {
        return Reply(false, "stale_session", "会话已过期");
    }
    if (_state == State::Transcribing && !_recognitionRequest) {
        return Reply(true);
    }
    if (_state != State::Recording || _phase != "recording") {
        return Reply(false, "not_recording", "当前未在录音");
    }
    _recordedMs = _elapsed.elapsed();
    _recordingLimit.stop();
    _progress.stop();
    Move(Event::ToggleRecording);
    _phase = "stopping";
    QByteArray pcm;
    QString error;
    const auto id = _session;
    const bool stopped = _capture.Stop(id, pcm, error);
    if (!IsCurrent(id)) {
        return Reply(false, _code, _error);
    }
    if (!stopped || pcm.isEmpty() || pcm.size() % 2 || pcm.size() > VoiceInputEvents::MaxAudioBytes) {
        Fail(id, "capture_failed", "音频采集失败或没有采集到音频");
        return Reply(false, _code, _error);
    }
    _phase = "transcribing";
    Publish();
    _asr.Recognize(id, pcm, _snapshot);
    return Reply(true);
}
QJsonObject VoiceInputController::Toggle()
{
    if (_state == State::Idle) {
        return StartRecording();
    }
    if (_state == State::Recording && _phase == "recording") {
        return StopRecording(QString::number(_session));
    }
    return Reply(false, "busy", "识别进行中，暂时无法切换录音");
}
QJsonObject VoiceInputController::Cancel(const QString& session)
{
    if (!_session || session != QString::number(_session)) {
        return Reply(false, "stale_session", "会话已过期");
    }
    if (_state == State::Idle) {
        return Reply(true);
    }
    if (_phase == "copying") {
        _copyState = "unknown";
    }
    Finish("cancelled", "cancelled", "语音输入已取消");
    return Reply(true);
}
void VoiceInputController::Recognize(RequestId request, const QByteArray& pcm)
{
    if (_shutdown || _state != State::Idle) {
        RespondRecognition(request, false, { }, "busy", "语音任务进行中");
        return;
    }
    if (pcm.isEmpty() || pcm.size() % 2 || pcm.size() > VoiceInputEvents::MaxAudioBytes) {
        RespondRecognition(request, false, { }, "invalid_audio", "无效或过大的 PCM16 音频");
        return;
    }
    QString error;
    if (!Prepare(error)) {
        RespondRecognition(request, false, { }, "not_configured", error);
        return;
    }
    _recognitionRequest = request;
    // 独立识别占用同一个任务槽，但不启动设备，也不自动复制。
    _state = State::Transcribing;
    _phase = "transcribing";
    Publish();
    _asr.Recognize(_session, pcm, _snapshot);
}
void VoiceInputController::CancelRequest(RequestId request)
{
    if (request && request == _recognitionRequest) {
        _recognitionRequest = 0; // 总线已终结该请求，不再响应。
        Finish("cancelled", "cancelled", "识别请求已取消");
    }
}
void VoiceInputController::RespondRecognition(RequestId request, bool ok, const QString& text, const QString& code, const QString& error)
{
    _endpoint.Respond(request,
        std::make_shared<ProcessorEvent>(VoiceInputEvents::ResultType, nullptr, nullptr,
            QVariantMap { { "ok", ok }, { "text", text }, { "session_id", QString::number(_session) }, { "error_code", code }, { "error", error } }));
}
void VoiceInputController::OnRecognized(quint64 session, const QString& text)
{
    if (!IsCurrent(session) || _phase != "transcribing") {
        return;
    }
    if (text.trimmed().isEmpty()) {
        Fail(session, "no_speech", "未检测到语音");
        return;
    }
    if (text.toUtf8().size() > ClipboardEvents::MaxTextBytes) {
        Fail(session, "result_too_large", "识别结果过大");
        return;
    }
    _text = text;
    if (_recognitionRequest) {
        Finish("recognized");
        return;
    }
    CopyResult(session);
}
QJsonObject VoiceInputController::RetryCopy(const QString& session)
{
    if (_shutdown) {
        return Reply(false, "unavailable", "语音输入服务已停止");
    }
    if (_state != State::Idle) {
        return Reply(false, "busy", "语音任务进行中");
    }
    if (!_session || session != QString::number(_session)) {
        return Reply(false, "stale_session", "会话已过期");
    }
    if (_text.isEmpty()) {
        return Reply(false, "no_result", "当前会话没有可复制的识别结果");
    }
    _state = State::Transcribing;
    _outcome.clear();
    _code.clear();
    _error.clear();
    CopyResult(_session);
    return Reply(true);
}
void VoiceInputController::CopyResult(quint64 session)
{
    _phase = "copying";
    _copyState = "pending";
    Publish();
    const QPointer<VoiceInputController> self(this);
    _clipboardRequest = _endpoint.Request(
        ClipboardEvents::WriteTextCapability,
        std::make_shared<ProcessorEvent>(ClipboardEvents::WriteText, nullptr, nullptr, QVariantMap { { "text", _text }, { "requestToken", QString::number(session) } }),
        [self, session](const RequestResult& result) {
            if (!self || !self->IsCurrent(session) || self->_phase != "copying" || result.requestId != self->_clipboardRequest) {
                return;
            }
            self->_clipboardRequest = 0;
            const bool valid = result.status == RequestStatus::Replied && result.payload && result.payload->type == ClipboardEvents::WriteResult
                && result.payload->data.value("ok").metaType().id() == QMetaType::Bool;
            if (valid && result.payload->data.value("ok").toBool() && result.payload->data.value("copy_state").toString() == "committed") {
                self->_copyState = "committed";
                self->Finish("copied");
            } else {
                self->_copyState = result.status == RequestStatus::NoProvider || result.status == RequestStatus::AmbiguousProvider ? "failed" : "unknown";
                if (valid && result.payload->data.value("copy_state").toString() == "failed") {
                    self->_copyState = "failed";
                }
                self->Finish("recognized_not_copied", "clipboard_failed", "识别完成，但未能确认写入剪贴板，可手动重试复制");
            }
        },
        ClipboardEvents::DeadlineMs + 1000);
    if (!_clipboardRequest) {
        _copyState = "failed";
        Finish("recognized_not_copied", "clipboard_unavailable", "剪贴板服务不可用");
    }
}
void VoiceInputController::Fail(quint64 session, const QString& code, const QString& error)
{
    if (!IsCurrent(session)) {
        return;
    }
    Finish("failed", code, error);
}
void VoiceInputController::Finish(const QString& outcome, const QString& code, const QString& error)
{
    const auto direct = _recognitionRequest;
    const auto copy = _clipboardRequest;
    _recognitionRequest = 0;
    _clipboardRequest = 0;
    if (_state == State::Recording && _elapsed.isValid()) {
        _recordedMs = _elapsed.elapsed();
    }
    if (_state == State::Transcribing) {
        Move(code.isEmpty() ? Event::TranscriptionComplete : Event::TranscriptionFailed);
    } else {
        _state = State::Idle; // 原状态机之外的设备失败/显式取消复位。
    }
    _phase = "idle";
    _outcome = outcome;
    _code = code;
    _error = error;
    _recordingLimit.stop();
    _progress.stop();
    _capture.Cancel();
    _asr.Cancel();
    if (copy && !_shutdown) {
        _endpoint.Cancel(copy);
    }
    _snapshot.apiKey.clear();
    if (direct && !_shutdown) {
        RespondRecognition(direct, code.isEmpty(), _text, code, error);
    }
    if (!_shutdown) {
        Publish(true);
    }
}
void VoiceInputController::Publish(bool completed)
{
    if (_shutdown) {
        return;
    }
    const auto status = Status();
    _endpoint.Notify(
        completed ? VoiceInputEvents::Completed : VoiceInputEvents::StateChanged, std::make_shared<ProcessorEvent>("voice_input.status", nullptr, nullptr, status.toVariantMap()));
    emit StateChanged(status);
}
void VoiceInputController::Shutdown()
{
    if (_shutdown) {
        return;
    }
    _shutdown = true;
    Finish("stopped", "unavailable", "语音输入服务已停止");
}
