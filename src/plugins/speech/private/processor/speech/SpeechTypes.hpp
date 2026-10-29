#pragma once

#include <QMetaType>
#include <QString>

using SpeechId = quint64;

struct SpeechOptions
{
    QString voiceId = QStringLiteral("female-tianmei");
    QString model = QStringLiteral("speech-2.8-turbo");
    double speed = 1.0;
    double volume = 1.0;
    int pitch = 0;
};

struct SpeechRequest
{
    QString text;
    SpeechOptions options;
};

enum class SpeechErrorCode {
    InvalidRequest, MissingApiKey, Authentication, Network, Timeout,
    Protocol, Synthesis, AudioDevice, BufferOverflow
};

struct SpeechError
{
    SpeechErrorCode code;
    QString message;
};

Q_DECLARE_METATYPE(SpeechRequest)
Q_DECLARE_METATYPE(SpeechError)
