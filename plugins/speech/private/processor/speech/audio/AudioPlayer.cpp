#include "AudioPlayer.hpp"
#include <QAudioDevice>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>

AudioPlayer::AudioPlayer(QObject* parent) : QObject(parent)
{
    _pump.setInterval(10);
    connect(&_pump, &QTimer::timeout, this, &AudioPlayer::Pump);
}

AudioPlayer::~AudioPlayer() { Stop(_id); }

void AudioPlayer::Start(SpeechId id, QAudioFormat format)
{
    Stop(_id);
    _id = id;
    _format = format;
    const auto device = QMediaDevices::defaultAudioOutput();
    if (device.isNull() || !device.isFormatSupported(format)) {
        Fail(SpeechErrorCode::AudioDevice, QStringLiteral("没有支持语音格式的音频输出设备")); return;
    }
    _sink.reset(new QAudioSink(device, format));
    _sink->setBufferSize(format.bytesForDuration(100000));
    _output = _sink->start();
    if (!_output) {
        Fail(SpeechErrorCode::AudioDevice, QStringLiteral("无法启动音频输出")); return;
    }
    _pump.start();
}

void AudioPlayer::Append(SpeechId id, QByteArray pcm)
{
    if (!id || id != _id || _finished) return;
    if (_pending.size() + pcm.size() > 8 * 1024 * 1024) {
        Fail(SpeechErrorCode::BufferOverflow, QStringLiteral("语音播放缓冲区已满")); return;
    }
    _pending.append(pcm);
    Pump();
}

void AudioPlayer::FinishInput(SpeechId id)
{
    if (!id || id != _id) return;
    if (_pending.size() % _format.bytesPerFrame()) {
        Fail(SpeechErrorCode::Protocol, QStringLiteral("PCM 音频帧不完整")); return;
    }
    _finished = true;
    Pump();
}

void AudioPlayer::Stop(SpeechId id)
{
    if (id != _id) return;
    _id = 0;
    _pump.stop();
    if (_sink) {
        _sink->reset();
        _sink.reset();
    }
    _output = nullptr;
    _pending.clear();
    _finished = _started = false;
}

void AudioPlayer::Fail(SpeechErrorCode code, const QString& message)
{
    const auto id = _id;
    Stop(id);
    emit Failed(id, {code, message});
}

void AudioPlayer::Pump()
{
    if (!_id || !_sink) return;
    const auto error = _sink->error();
    if (error == QAudio::OpenError || error == QAudio::IOError || error == QAudio::FatalError) {
        Fail(SpeechErrorCode::AudioDevice, QStringLiteral("音频输出设备发生错误")); return;
    }
    const auto id = _id;
    const auto frame = _format.bytesPerFrame();
    const auto available = qMin<qsizetype>(_pending.size(), _sink->bytesFree());
    const auto count = available - available % frame;
    if (count > 0) {
        const auto written = _output->write(_pending.constData(), count);
        if (written < 0) { Fail(SpeechErrorCode::AudioDevice, QStringLiteral("音频写入失败")); return; }
        _pending.remove(0, written);
    }
    if (!_started && _sink->state() == QAudio::ActiveState) {
        _started = true;
        emit PlaybackStarted(id);
        if (_id != id) return;
    }
    if (_finished && _pending.isEmpty() && _sink->state() == QAudio::IdleState) {
        Stop(id);
        emit Drained(id);
    }
}
