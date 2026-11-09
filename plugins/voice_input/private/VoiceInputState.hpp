#pragma once

// 从 VoiceInput/src/state.rs 迁移的纯三态控制机；不执行设备/网络副作用。
namespace VoiceInputState {
enum class State { Idle, Recording, Transcribing };
enum class Event { ToggleRecording, TranscriptionComplete, TranscriptionFailed };
bool Transition(State current, Event event, State& next);
const char* Name(State state);
}
