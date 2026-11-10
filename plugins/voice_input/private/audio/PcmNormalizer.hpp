#pragma once
#include <QAudioFormat>
#include <QByteArray>
#include <QString>
#include <samplerate.h>

// Incremental native-format to 16 kHz mono PCM16 little-endian conversion.
class PcmNormalizer
{
public:
    ~PcmNormalizer();
    PcmNormalizer() = default;
    PcmNormalizer(const PcmNormalizer&) = delete;
    PcmNormalizer& operator=(const PcmNormalizer&) = delete;
    bool Reset(const QAudioFormat& format, QString& error);
    bool Append(const QByteArray& bytes, QString& error);
    bool Finish(QByteArray& pcm, QString& error);
    void Clear();
private:
    bool Convert(const float* samples, long frames, bool final, QString& error);
    bool Store(const float* samples, long count, QString& error);
    QAudioFormat _format;
    SRC_STATE* _resampler = nullptr;
    QByteArray _pending, _output;
    bool _active = false;
};
