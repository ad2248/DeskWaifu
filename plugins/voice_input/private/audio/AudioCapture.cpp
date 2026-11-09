#include "AudioCapture.hpp"
#include <QAudioDevice>
#include <QJsonObject>
#include <QMediaDevices>

AudioCapture::AudioCapture(QObject* parent) : QObject(parent)
{
    _limit.setSingleShot(true);
    connect(&_limit, &QTimer::timeout, this, [this] {
        if (_active) Fail(QStringLiteral("audio_limit"), QStringLiteral("Recording duration limit exceeded"));
    });
}
AudioCapture::~AudioCapture() { AudioCapture::Cancel(); }
QJsonObject AudioCapture::DeviceInfo(const QByteArray& id, const QString& name, bool isDefault)
{
    return {{"id", QString::fromLatin1(id.toBase64(QByteArray::Base64UrlEncoding))},
        {"name", name}, {"is_default", isDefault}};
}
QJsonArray AudioCapture::ListDevices()
{
    QJsonArray devices;
    const auto defaultId = QMediaDevices::defaultAudioInput().id();
    for (const auto& device : QMediaDevices::audioInputs())
        devices.append(DeviceInfo(device.id(), device.description(), device.id() == defaultId));
    return devices;
}
bool AudioCapture::Start(quint64 sessionId, const QString& deviceId, int maxSeconds, QString& error)
{
    if (_active) { error = QStringLiteral("Audio capture is busy"); return false; }
    if (maxSeconds < 1 || maxSeconds > 300) { error = QStringLiteral("Invalid recording duration"); return false; }
    Cancel();
    QAudioDevice selected;
    if (deviceId.isEmpty()) selected = QMediaDevices::defaultAudioInput();
    else for (const auto& device : QMediaDevices::audioInputs())
        if (QString::fromLatin1(device.id().toBase64(QByteArray::Base64UrlEncoding)) == deviceId) { selected = device; break; }
    if (selected.isNull()) { error = QStringLiteral("Audio input device is unavailable"); return false; }
    QAudioFormat format;
    format.setSampleRate(16000); format.setChannelCount(1); format.setSampleFormat(QAudioFormat::Int16);
    if (!selected.isFormatSupported(format)) format = selected.preferredFormat();
    if (!_normalizer.Reset(format, error)) return false;
    _source = std::make_unique<QAudioSource>(selected, format);
    _source->setBufferSize(format.bytesForDuration(100000));
    _sessionId = sessionId; _maxSeconds = maxSeconds;
    _maxInputBytes = qint64(maxSeconds) * format.sampleRate() * format.bytesPerFrame();
    _inputBytes = 0; _budgetReached = false; _active = true;
    const auto generation = _generation;
    connect(_source.get(), &QAudioSource::stateChanged, this, [this, generation](QAudio::State state) {
        if (_generation == generation && _active && state == QAudio::StoppedState && _source->error() != QAudio::NoError) {
            QueueFailure(QStringLiteral("audio_device"), QStringLiteral("Audio input device failed"));
        }
    });
    _device = _source->start();
    if (!_device || _source->error() != QAudio::NoError) {
        Cancel(); error = QStringLiteral("Could not start audio input"); return false;
    }
    _elapsed.start();
    // Controller stops normally at maxSeconds; this independent guard allows one scheduling grace second.
    _limit.start((maxSeconds + 1) * 1000);
    connect(_device, &QIODevice::readyRead, this, [this, generation] {
        if (_generation == generation) OnAudioReady();
    });
    error.clear(); return true;
}
void AudioCapture::OnAudioReady()
{
    if (!_active || !_pendingFailure.isEmpty()) return;
    QString message;
    if (!Drain(message)) QueueFailure(QStringLiteral("audio_limit"), message);
}
void AudioCapture::QueueFailure(const QString& code, const QString& message)
{
    if (!_active || !_pendingFailure.isEmpty()) return;
    // Neither QAudioSource nor its pull device may be destroyed inside their signal stack.
    // Mark the error synchronously so Stop cannot upload a partial recording before cleanup runs.
    _pendingFailure = message;
    _limit.stop();
    const auto id = _sessionId;
    const auto generation = _generation;
    QMetaObject::invokeMethod(this, [this, id, generation, code, message] {
        if (_active && _sessionId == id && _generation == generation) Fail(code, message);
    }, Qt::QueuedConnection);
}
bool AudioCapture::Drain(QString& error)
{
    if (_elapsed.isValid() && _elapsed.elapsed() > qint64(_maxSeconds + 1) * 1000) {
        error = QStringLiteral("Recording duration limit exceeded"); return false;
    }
    // Drain only the bytes available at this boundary, not an indefinitely replenished stream.
    qint64 available = _device ? _device->bytesAvailable() : 0;
    while (_device && available > 0) {
        if (_elapsed.isValid() && _elapsed.elapsed() > qint64(_maxSeconds + 1) * 1000) {
            error = QStringLiteral("Recording duration limit exceeded"); return false;
        }
        const auto bytes = _device->read(qMin<qint64>(65536, available));
        if (bytes.isEmpty()) break;
        available -= bytes.size();
        // The budget is an integral number of native frames. Append may carry partial frames
        // across reads, but reaching this exact byte boundary always completes the last frame.
        const auto accepted = qMin<qint64>(bytes.size(), _maxInputBytes - _inputBytes);
        if (accepted > 0 && !_normalizer.Append(bytes.first(accepted), error)) return false;
        _inputBytes += accepted;
        if (_inputBytes == _maxInputBytes) _budgetReached = true;
        // Excess samples are normal timer/readyRead scheduling jitter, not capture failure.
        // Continue draining/discarding until the controller's normal stop or the watchdog.
    }
    return true;
}
bool AudioCapture::Stop(quint64 sessionId, QByteArray& pcm, QString& error)
{
    if (!_active || sessionId != _sessionId) { error = QStringLiteral("Stale audio capture session"); return false; }
    if (!_pendingFailure.isEmpty()) { error = _pendingFailure; return false; }
    // The pull device must remain live until every available tail byte has been read.
    const bool drained = Drain(error);
    _active = false; _limit.stop();
    if (_source) { _source->disconnect(this); _source->stop(); }
    _device = nullptr; _source.reset();
    if (!drained) { _normalizer.Clear(); return false; }
    const bool ok = _normalizer.Finish(pcm, error);
    if (!ok) _normalizer.Clear();
    return ok;
}
void AudioCapture::Cancel()
{
    ++_generation; _pendingFailure.clear(); _budgetReached = false;
    _active = false; _limit.stop();
    if (_source) { _source->disconnect(this); _source->reset(); }
    _device = nullptr; _source.reset(); _normalizer.Clear(); _elapsed.invalidate();
}
void AudioCapture::Fail(const QString& code, const QString& message)
{
    const auto id = _sessionId;
    Cancel(); emit Failed(id, code, message);
}
