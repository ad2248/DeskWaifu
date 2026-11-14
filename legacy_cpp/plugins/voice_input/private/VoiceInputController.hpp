#pragma once

#include "VoiceInputSettings.hpp"
#include "VoiceInputState.hpp"
#include "event/EventEndpoint.hpp"
#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QTimer>

class AudioCapture;
class NativeAsrClient;

class VoiceInputController final : public QObject {
    Q_OBJECT
public:
    VoiceInputController(EventEndpoint& endpoint, VoiceInputSettings& settings, AudioCapture& capture, NativeAsrClient& asr, QObject* parent = nullptr);
    ~VoiceInputController() override;
    QJsonObject StartRecording();
    QJsonObject StopRecording(const QString& session);
    QJsonObject Toggle();
    QJsonObject RetryCopy(const QString& session);
    QJsonObject Cancel(const QString& session);
    QJsonObject Status() const;
    QJsonObject Result(const QString& session) const;
    void Recognize(RequestId request, const QByteArray& pcm);
    void CancelRequest(RequestId request);
    void Shutdown();
signals:
    void StateChanged(const QJsonObject& status);

private:
    using State = VoiceInputState::State;
    using Event = VoiceInputState::Event;
    QJsonObject Reply(bool ok, const QString& code = { }, const QString& error = { }) const;
    bool Prepare(QString& error);
    bool Move(Event event);
    bool IsCurrent(quint64 session) const;
    void Publish(bool completed = false);
    void OnRecognized(quint64 session, const QString& text);
    void CopyResult(quint64 session);
    void Fail(quint64 session, const QString& code, const QString& error);
    void Finish(const QString& outcome, const QString& code = { }, const QString& error = { });
    void RespondRecognition(RequestId request, bool ok, const QString& text, const QString& code, const QString& error);

    EventEndpoint& _endpoint;
    VoiceInputSettings& _settings;
    AudioCapture& _capture;
    NativeAsrClient& _asr;
    VoiceInputConfig _snapshot;
    State _state = State::Idle;
    QString _phase = "idle", _text, _outcome, _code, _error, _copyState = "not_requested";
    quint64 _nextSession = 0, _session = 0;
    RequestId _recognitionRequest = 0, _clipboardRequest = 0;
    QTimer _recordingLimit, _progress;
    QElapsedTimer _elapsed;
    qint64 _recordedMs = 0;
    bool _shutdown = false;
};
