use voice_domain::{transition, Event, State};

#[test]
fn transition_matrix_matches_original_state_machine() {
    let cases = [
        (State::Idle, Event::ToggleRecording, true, State::Recording),
        (
            State::Idle,
            Event::TranscriptionComplete,
            false,
            State::Idle,
        ),
        (State::Idle, Event::TranscriptionFailed, false, State::Idle),
        (
            State::Recording,
            Event::ToggleRecording,
            true,
            State::Transcribing,
        ),
        (
            State::Recording,
            Event::TranscriptionComplete,
            false,
            State::Recording,
        ),
        (
            State::Recording,
            Event::TranscriptionFailed,
            false,
            State::Recording,
        ),
        (
            State::Transcribing,
            Event::ToggleRecording,
            false,
            State::Transcribing,
        ),
        (
            State::Transcribing,
            Event::TranscriptionComplete,
            true,
            State::Idle,
        ),
        (
            State::Transcribing,
            Event::TranscriptionFailed,
            true,
            State::Idle,
        ),
    ];

    for (initial, event, expected_validity, expected_state) in cases {
        let mut state = initial;
        assert_eq!(transition(&mut state, event), expected_validity);
        assert_eq!(state, expected_state);
    }
}

#[test]
fn state_names_match_original() {
    assert_eq!(State::Idle.name(), "idle");
    assert_eq!(State::Recording.name(), "recording");
    assert_eq!(State::Transcribing.name(), "transcribing");
}
