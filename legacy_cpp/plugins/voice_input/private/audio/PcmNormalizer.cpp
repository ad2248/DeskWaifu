#include "PcmNormalizer.hpp"
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {
constexpr qsizetype MaxPcmBytes = 300 * 16000 * 2;
}
PcmNormalizer::~PcmNormalizer() { Clear(); }
void PcmNormalizer::Clear()
{
    if (_resampler) {
        _resampler = src_delete(_resampler);
    }
    _pending.clear();
    _output.clear();
    _active = false;
}
bool PcmNormalizer::Reset(const QAudioFormat& format, QString& error)
{
    Clear();
    if (!format.isValid() || format.channelCount() > 32 || format.sampleRate() > 384000
        || (format.sampleFormat() != QAudioFormat::UInt8 && format.sampleFormat() != QAudioFormat::Int16 && format.sampleFormat() != QAudioFormat::Int32
            && format.sampleFormat() != QAudioFormat::Float)) {
        error = QStringLiteral("Unsupported input audio format");
        return false;
    }
    _format = format;
    if (format.sampleRate() != 16000) {
        int code = 0;
        _resampler = src_new(SRC_SINC_FASTEST, 1, &code);
        if (!_resampler) {
            error = QStringLiteral("Audio resampler initialization failed");
            return false;
        }
    }
    _active = true;
    error.clear();
    return true;
}
bool PcmNormalizer::Store(const float* samples, long count, QString& error)
{
    if (count > (MaxPcmBytes - _output.size()) / 2) {
        error = QStringLiteral("Audio buffer limit exceeded");
        return false;
    }
    for (long i = 0; i < count; ++i) {
        const float value = std::isfinite(samples[i]) ? std::clamp(samples[i], -1.0f, 1.0f) : 0.0f;
        const qint16 pcm = qint16(std::clamp(std::lround(value * 32768.0f), -32768L, 32767L));
        const auto le = qToLittleEndian(pcm);
        _output.append(reinterpret_cast<const char*>(&le), 2);
    }
    return true;
}
bool PcmNormalizer::Convert(const float* samples, long frames, bool final, QString& error)
{
    if (!_resampler) {
        return Store(samples, frames, error);
    }
    float output[4096];
    long used = 0;
    do {
        SRC_DATA data { };
        data.data_in = samples + used;
        data.input_frames = frames - used;
        data.data_out = output;
        data.output_frames = 4096;
        data.src_ratio = 16000.0 / _format.sampleRate();
        data.end_of_input = final;
        if (src_process(_resampler, &data) != 0) {
            error = QStringLiteral("Audio resampling failed");
            return false;
        }
        used += data.input_frames_used;
        if (!Store(output, data.output_frames_gen, error)) {
            return false;
        }
        if (used == frames && (!final || data.output_frames_gen == 0)) {
            break;
        }
        if (!data.input_frames_used && !data.output_frames_gen) {
            error = QStringLiteral("Audio resampling stalled");
            return false;
        }
    } while (true);
    return true;
}
bool PcmNormalizer::Append(const QByteArray& bytes, QString& error)
{
    if (!_active) {
        error = QStringLiteral("Audio normalizer is inactive");
        return false;
    }
    // Bound each call as well as total output; a malformed producer cannot allocate unbounded input.
    if (bytes.size() > 4 * 1024 * 1024) {
        error = QStringLiteral("Audio input block too large");
        return false;
    }
    _pending += bytes;
    const int frameSize = _format.bytesPerFrame();
    const qsizetype complete = _pending.size() / frameSize * frameSize;
    if (_format.sampleRate() == 16000 && _format.channelCount() == 1 && _format.sampleFormat() == QAudioFormat::Int16) {
        if (complete > MaxPcmBytes - _output.size()) {
            error = QStringLiteral("Audio buffer limit exceeded");
            return false;
        }
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
        _output += _pending.first(complete);
#else
        for (qsizetype i = 0; i < complete; i += 2) {
            _output += _pending[i + 1];
            _output += _pending[i];
        }
#endif
    } else {
        const int channels = _format.channelCount();
        const int sampleSize = _format.bytesPerSample();
        // Keep conversion work bounded even for high-rate, many-channel devices.
        for (qsizetype offset = 0; offset < complete;) {
            const auto count = std::min<qsizetype>(4096, (complete - offset) / frameSize);
            std::vector<float> mono(size_t(count), 0.0f);
            for (qsizetype f = 0; f < count; ++f) {
                double sum = 0;
                for (int c = 0; c < channels; ++c) {
                    const char* p = _pending.constData() + offset + f * frameSize + c * sampleSize;
                    float value = 0;
                    switch (_format.sampleFormat()) {
                    case QAudioFormat::UInt8:
                        value = (quint8(*p) - 128) / 128.0f;
                        break;
                    case QAudioFormat::Int16: {
                        qint16 v;
                        std::memcpy(&v, p, 2);
                        value = v / 32768.0f;
                        break;
                    }
                    case QAudioFormat::Int32: {
                        qint32 v;
                        std::memcpy(&v, p, 4);
                        value = float(v / 2147483648.0);
                        break;
                    }
                    case QAudioFormat::Float:
                        std::memcpy(&value, p, 4);
                        break;
                    default:
                        break;
                    }
                    sum += std::isfinite(value) ? value : 0;
                }
                mono[size_t(f)] = float(sum / channels);
            }
            if (!Convert(mono.data(), long(count), false, error)) {
                return false;
            }
            offset += count * frameSize;
        }
    }
    _pending.remove(0, complete);
    error.clear();
    return true;
}
bool PcmNormalizer::Finish(QByteArray& pcm, QString& error)
{
    if (!_active || !_pending.isEmpty()) {
        error = QStringLiteral("Incomplete audio sample frame");
        return false;
    }
    const float dummy = 0;
    if (!Convert(&dummy, 0, true, error)) {
        return false;
    }
    pcm = std::move(_output);
    Clear();
    error.clear();
    return true;
}
