#include "WavEncoder.hpp"
#include <QtEndian>

QByteArray WavEncoder::Encode(const QByteArray& pcm, QString& error, int trailingSilenceMs)
{
    if (pcm.size() % 2 || pcm.size() > 300 * 32000 || trailingSilenceMs < 0 || trailingSilenceMs > 1000) {
        error = QStringLiteral("Invalid PCM audio"); return {};
    }
    const quint32 size = quint32(pcm.size() + trailingSilenceMs * 32);
    QByteArray wav;
    wav.reserve(44 + size);
    auto u16 = [&wav](quint16 n) { n = qToLittleEndian(n); wav.append(reinterpret_cast<const char*>(&n), 2); };
    auto u32 = [&wav](quint32 n) { n = qToLittleEndian(n); wav.append(reinterpret_cast<const char*>(&n), 4); };
    wav += "RIFF"; u32(36 + size); wav += "WAVEfmt "; u32(16); u16(1); u16(1);
    u32(16000); u32(32000); u16(2); u16(16); wav += "data"; u32(size);
    wav += pcm; wav += QByteArray(trailingSilenceMs * 32, '\0');
    error.clear(); return wav;
}
