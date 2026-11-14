#include "VoiceInputState.hpp"

namespace VoiceInputState {
bool Transition(State current, Event event, State& next)
{
    next = current;
    if (event == Event::ToggleRecording && current == State::Idle) {
        next = State::Recording;
    } else if (event == Event::ToggleRecording && current == State::Recording) {
        next = State::Transcribing;
    } else if (current == State::Transcribing && (event == Event::TranscriptionComplete || event == Event::TranscriptionFailed)) {
        next = State::Idle;
    } else {
        return false;
    }
    return true;
}
const char* Name(State state)
{
    switch (state) {
    case State::Idle:
        return "idle";
    case State::Recording:
        return "recording";
    case State::Transcribing:
        return "transcribing";
    }
    return "idle";
}
}
