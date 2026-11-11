#include "VoiceInputState.hpp"
#include <iostream>
#include <stdexcept>

int main()
try {
    using namespace VoiceInputState;
    const State states[] { State::Idle, State::Recording, State::Transcribing };
    const Event events[] { Event::ToggleRecording, Event::TranscriptionComplete, Event::TranscriptionFailed };
    for (const auto state : states) {
        for (const auto event : events) {
            State next = State::Idle;
            const bool legal = (event == Event::ToggleRecording && state != State::Transcribing) || (event != Event::ToggleRecording && state == State::Transcribing);
            if (Transition(state, event, next) != legal) {
                throw std::runtime_error("original state/event acceptance changed");
            }
            if (!legal && next != state) {
                throw std::runtime_error("invalid transition changed state");
            }
            if (legal) {
                const auto expected = event != Event::ToggleRecording ? State::Idle : state == State::Idle ? State::Recording : State::Transcribing;
                if (next != expected) {
                    throw std::runtime_error("original transition destination changed");
                }
            }
        }
    }
    std::cout << "VoiceInput original three-state transition matrix passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}
