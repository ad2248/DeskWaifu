//! Speech playback state machine.
//!
//! Pure port of `plugins/speech/private/processor/speech/SpeechPlayer.cpp`
//! (see SP-02/SP-03/SP-04). It keeps only the transitions, the speech id
//! lifecycle and the observable outcomes: no Qt state machine, audio device,
//! network or threading. Source and audio objects are driven by the owner of
//! this machine from the ids and outcomes reported here.
//!
//! Transitions (identical to the C++ machine):
//!
//! | from                       | event     | guard  | to       | outcome                       |
//! |----------------------------|-----------|--------|----------|-------------------------------|
//! | Idle/Preparing/Playing/Draining | speak | text non-blank | Preparing | Finished(Replaced) if active, Accepted(id) |
//! | Preparing/Playing/Draining | stop      | -      | Idle     | Finished(Stopped)             |
//! | Preparing/Playing/Draining | failed    | active | Idle     | Error(id), Finished(Failed)   |
//! | Preparing                  | started   | active | Playing  | -                             |
//! | Preparing/Playing          | completed | active | Draining | -                             |
//! | Draining                   | drained   | active | Idle     | Finished(Completed)           |
//! | Idle                       | stop/failed/started/completed/drained | - | Idle | - (ignored) |
//!
//! `Ready` is not a state event in C++ either; the source only starts audio
//! for the active id. Late events of a replaced id are dropped everywhere.

use crate::Error;

/// Failure reported to the state machine; mirrors the C++ `SpeechErrorCode`
/// split between source failures and local audio failures.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum PlayerError {
    /// Synthesis/source failure, classified by the protocol layer.
    Source(Error),
    /// Local audio device failure (`SpeechErrorCode::AudioDevice`).
    AudioDevice,
    /// Local audio buffer overflow (`SpeechErrorCode::BufferOverflow`).
    BufferOverflow,
}

/// Playback state; mirrors `SpeechPlayer::State`.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum State {
    /// No playback; [`Player::id`] is 0.
    #[default]
    Idle,
    /// Request accepted, waiting for the source to start audio.
    Preparing,
    /// Audio is playing.
    Playing,
    /// Synthesis finished; waiting for the audio output to drain.
    Draining,
}

/// Why a playback ended; mirrors `SpeechPlayer::FinishReason`.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum FinishReason {
    /// Audio drained after synthesis completed.
    Completed,
    /// Cancelled by [`Player::stop`].
    Stopped,
    /// Superseded by a newer non-blank request.
    Replaced,
    /// Source or audio failure.
    Failed,
}

/// Deterministic output of one transition, in emission order.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Outcome {
    /// A request was accepted and assigned this id.
    Accepted { id: u64 },
    /// A state was entered, in Qt's sense: also emitted when a transition
    /// leaves and re-enters the same state.
    StateChanged(State),
    /// A playback ended; exactly one per accepted id.
    Finished { id: u64, reason: FinishReason },
    /// The active playback failed; always followed by `Finished { reason: Failed }`.
    Error { id: u64, error: PlayerError },
}

/// Speech playback state machine driven synchronously by its owner.
#[derive(Debug, Default)]
pub struct Player {
    state: State,
    id: u64,
    next_id: u64,
    outcomes: Vec<Outcome>,
}

impl Player {
    /// Creates an idle machine.
    ///
    /// The C++ machine also enters `Idle` while starting, but every consumer
    /// connects after construction, so that initial entry is unobservable
    /// there; here it emits nothing and `Idle` is simply the initial state.
    pub fn new() -> Self {
        Self::default()
    }

    /// Effective state.
    pub const fn state(&self) -> State {
        self.state
    }

    /// Current accepted speech id, 0 when idle.
    pub const fn id(&self) -> u64 {
        self.id
    }

    /// Removes and returns the outcomes emitted since the last call.
    pub fn take_outcomes(&mut self) -> Vec<Outcome> {
        std::mem::take(&mut self.outcomes)
    }

    /// Accepts a non-blank request, replacing any active playback.
    ///
    /// Returns the new speech id, or `None` for blank text, which leaves the
    /// current playback untouched and allocates no id.
    ///
    /// Blank matches the C++ `request.text.trimmed().isEmpty()`: Qt trims
    /// characters for which `QChar::isSpace` holds, which is the Unicode
    /// `White_Space` property, exactly what [`str::trim`] trims.
    pub fn speak(&mut self, text: &str) -> Option<u64> {
        if text.trim().is_empty() {
            return None;
        }
        self.finish(FinishReason::Replaced);
        self.next_id += 1;
        self.id = self.next_id;
        self.outcomes.push(Outcome::Accepted { id: self.id });
        self.enter(State::Preparing);
        Some(self.id)
    }

    /// Requests a stop of the active playback; a no-op while idle.
    pub fn stop(&mut self) {
        if self.state == State::Idle {
            return;
        }
        self.finish(FinishReason::Stopped);
        self.enter(State::Idle);
    }

    /// Reports that the source is ready for `id`.
    ///
    /// The pure machine does not change state here; the returned flag is the
    /// C++ `id == _id && id` check that decides whether audio may start.
    pub fn ready(&mut self, id: u64) -> bool {
        self.active(id)
    }

    /// Reports that audio output started for `id`.
    pub fn started(&mut self, id: u64) {
        if self.active(id) && self.state == State::Preparing {
            self.enter(State::Playing);
        }
    }

    /// Reports that the source finished sending audio for `id`.
    pub fn completed(&mut self, id: u64) {
        if self.active(id) && matches!(self.state, State::Preparing | State::Playing) {
            self.enter(State::Draining);
        }
    }

    /// Reports that the audio output drained for `id`; the only path to
    /// `Finished(Completed)`.
    pub fn drained(&mut self, id: u64) {
        if !self.active(id) || self.state != State::Draining {
            return;
        }
        self.finish(FinishReason::Completed);
        self.enter(State::Idle);
    }

    /// Reports a source or audio failure for `id`; stale ids are ignored.
    pub fn failed(&mut self, id: u64, error: PlayerError) {
        if !self.active(id) {
            return;
        }
        self.outcomes.push(Outcome::Error { id, error });
        self.finish(FinishReason::Failed);
        self.enter(State::Idle);
    }

    fn active(&self, id: u64) -> bool {
        id != 0 && id == self.id
    }

    /// Clears the active id before emitting, so re-entrant callbacks cannot see
    /// a finished playback as current (as in the C++ `End`).
    fn finish(&mut self, reason: FinishReason) {
        let id = std::mem::take(&mut self.id);
        if id != 0 {
            self.outcomes.push(Outcome::Finished { id, reason });
        }
    }

    fn enter(&mut self, state: State) {
        // Qt's transitions are external, so a state that is both source and
        // target (only `speak` while Preparing) is left and re-entered, which
        // emits `StateChanged` again. Every other caller always changes state.
        self.state = state;
        self.outcomes.push(Outcome::StateChanged(state));
    }
}
