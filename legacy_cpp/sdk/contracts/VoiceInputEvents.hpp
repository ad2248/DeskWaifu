#pragma once

// 控制/结果跨插件传递；连续 PCM 只在采集插件内部流转。
namespace VoiceInputEvents {
inline constexpr auto Start = "voice_input.start/v1";
inline constexpr auto Stop = "voice_input.stop/v1";
inline constexpr auto Toggle = "voice_input.toggle/v1";
inline constexpr auto Cancel = "voice_input.cancel/v1";
inline constexpr auto Status = "voice_input.status/v1";
inline constexpr auto Result = "voice_input.result/v1";
inline constexpr auto Recognize = "voice_input.recognize/v1";
inline constexpr auto StateChanged = "voice_input.state_changed/v1";
inline constexpr auto Completed = "voice_input.completed/v1";
inline constexpr auto Control = "voice_input.control";
inline constexpr auto Audio = "voice_input.audio"; // QByteArray pcm: PCM16 LE, mono, 16000 Hz.
inline constexpr auto ResultType = "voice_input.result";
inline constexpr int SampleRate = 16000;
inline constexpr int MaxAudioBytes = 300 * SampleRate * 2;
}
