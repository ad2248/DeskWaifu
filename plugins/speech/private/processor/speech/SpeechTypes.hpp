#pragma once

#include <QMetaType>
#include <QString>

// 播放器分配的播报标识；0 表示没有活动播报，不等同于总线请求 ID。
using SpeechId = quint64;

// MiniMax 合成参数；来源启动时校验语速、音量、音调及非空模型和音色。
struct SpeechOptions {
    QString voiceId = QStringLiteral("female-tianmei");
    QString model = QStringLiteral("speech-2.8-turbo");
    double speed = 1.0;
    double volume = 1.0;
    int pitch = 0;
};

// 可按值传递的合成输入，文本与参数由语音来源在任务期间持有。
struct SpeechRequest {
    QString text;
    SpeechOptions options;
};

// 区分输入、认证、传输、合成和本地播放失败，供业务层分类反馈。
enum class SpeechErrorCode { InvalidRequest, MissingApiKey, Authentication, Network, Timeout, Protocol, Synthesis, AudioDevice, BufferOverflow };

// 可跨 Qt 信号传递的错误分类与可展示消息，不应包含认证凭据。
struct SpeechError {
    SpeechErrorCode code;
    QString message;
};

Q_DECLARE_METATYPE(SpeechRequest)
Q_DECLARE_METATYPE(SpeechError)
