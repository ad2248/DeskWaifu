#pragma once
#include <QByteArray>
#include <QString>

class WavEncoder
{
public:
    // Empty return signals invalid PCM/alignment or excessive size; no input is modified.
    static QByteArray Encode(const QByteArray& pcm, QString& error, int trailingSilenceMs = 250);
};
