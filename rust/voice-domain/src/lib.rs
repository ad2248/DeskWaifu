#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum State {
    Idle,
    Recording,
    Transcribing,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Event {
    ToggleRecording,
    TranscriptionComplete,
    TranscriptionFailed,
}

impl State {
    pub const fn name(self) -> &'static str {
        match self {
            Self::Idle => "idle",
            Self::Recording => "recording",
            Self::Transcribing => "transcribing",
        }
    }
}

pub fn transition(state: &mut State, event: Event) -> bool {
    let next = match (*state, event) {
        (State::Idle, Event::ToggleRecording) => State::Recording,
        (State::Recording, Event::ToggleRecording) => State::Transcribing,
        (State::Transcribing, Event::TranscriptionComplete | Event::TranscriptionFailed) => {
            State::Idle
        }
        _ => return false,
    };

    *state = next;
    true
}
