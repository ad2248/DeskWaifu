//! External control-source arbitration for the desktop model.
//!
//! Mouth and gaze are arbitrated independently between two classes of source:
//! a shared persistent source written by one-shot CLI commands, and a stream
//! source owned by one live IPC connection. A live stream source outranks the
//! persistent source; within the same class the latest update wins. Releasing,
//! resetting, or disconnecting a source falls back to the next qualifying one.
//! A real model identity change clears every external source.

use std::collections::BTreeMap;

/// Independently arbitrated control channels.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Channel {
    Mouth,
    Look,
}

/// Identity of an external control source.
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum SourceId {
    /// Shared one-shot CLI source; survives the connection that wrote it.
    Persistent,
    /// Connection-owned streaming source; only valid while the connection lives.
    Connection(u64),
}

impl SourceId {
    fn streaming(self) -> bool {
        matches!(self, Self::Connection(_))
    }
}

/// Value written to one source for one channel.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum ChannelValue {
    /// Mouth opening in the inclusive `0.0..=1.0` range.
    Mouth(f32),
    /// Gaze target coordinates; both must be finite.
    Look(f32, f32),
}

/// Rejection reasons for an external control write; the state is unchanged.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ControlError {
    MouthOutOfRange,
    LookNotFinite,
}

/// Model identity that invalidates external sources and pinned tokens.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ModelIdentity {
    pub index: i32,
    pub token: String,
}

impl ModelIdentity {
    pub fn new(index: i32, token: impl Into<String>) -> Self {
        Self {
            index,
            token: token.into(),
        }
    }
}

/// Effective per-frame control values, as reported by `get_control_status`.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ControlStatus {
    pub has_mouth: bool,
    pub mouth: f32,
    pub has_look: bool,
    pub look_x: f32,
    pub look_y: f32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
struct Entry {
    active: bool,
    x: f32,
    y: f32,
    order: u64,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
struct Source {
    mouth: Entry,
    look: Entry,
}

impl Channel {
    fn of(self, source: &Source) -> &Entry {
        match self {
            Self::Mouth => &source.mouth,
            Self::Look => &source.look,
        }
    }

    fn of_mut(self, source: &mut Source) -> &mut Entry {
        match self {
            Self::Mouth => &mut source.mouth,
            Self::Look => &mut source.look,
        }
    }
}

/// Per-channel arbitration state. Not internally synchronized; the interaction
/// domain owns it and updates and reads it serially.
pub struct Arbiter {
    model: ModelIdentity,
    sources: BTreeMap<SourceId, Source>,
    order: u64,
}

impl Arbiter {
    pub fn new(model: ModelIdentity) -> Self {
        Self {
            model,
            sources: BTreeMap::new(),
            order: 0,
        }
    }

    /// Updates one channel of a source. Invalid values are rejected without
    /// modifying state. A source's class comes from its identity, so the most
    /// recent write for a source also decides where it ranks.
    pub fn update(&mut self, source: SourceId, value: ChannelValue) -> Result<(), ControlError> {
        let (channel, x, y) = match value {
            ChannelValue::Mouth(value) => {
                if !value.is_finite() || !(0.0..=1.0).contains(&value) {
                    return Err(ControlError::MouthOutOfRange);
                }
                (Channel::Mouth, value, 0.0)
            }
            ChannelValue::Look(x, y) => {
                if !x.is_finite() || !y.is_finite() {
                    return Err(ControlError::LookNotFinite);
                }
                (Channel::Look, x, y)
            }
        };
        self.order += 1;
        *channel.of_mut(self.sources.entry(source).or_default()) = Entry {
            active: true,
            x,
            y,
            order: self.order,
        };
        Ok(())
    }

    /// Clears one channel of a source; the other channel keeps its value.
    pub fn reset(&mut self, source: SourceId, channel: Channel) {
        if let Some(entry) = self.sources.get_mut(&source) {
            channel.of_mut(entry).active = false;
        }
    }

    /// Releases both channels of a source, restoring the next eligible source.
    pub fn release(&mut self, source: SourceId) {
        self.sources.remove(&source);
    }

    /// Releases the streaming controls owned by a closed connection. The
    /// persistent CLI source is a different identity and is never touched.
    pub fn disconnect(&mut self, connection: u64) {
        self.release(SourceId::Connection(connection));
    }

    /// Applies a completed model identity change and reports whether external
    /// sources were cleared. Selecting the current model changes nothing, and a
    /// failed selection must not reach this call, so both keep all sources.
    pub fn model_switch(&mut self, next: ModelIdentity) -> bool {
        if next == self.model {
            return false;
        }
        self.sources.clear();
        self.model = next;
        true
    }

    /// Token of the current model, as returned to clients that pin operations.
    pub fn model_token(&self) -> &str {
        &self.model.token
    }

    /// Whether `token` still names the current model. A token obtained before a
    /// model switch is stale and must be rejected; a missing token never matches.
    pub fn token_is_current(&self, token: &str) -> bool {
        token == self.model.token
    }

    /// Effective values of both channels after arbitration.
    pub fn status(&self) -> ControlStatus {
        let mut status = ControlStatus::default();
        if let Some(entry) = self.winner(Channel::Mouth) {
            status.has_mouth = true;
            status.mouth = entry.x;
        }
        if let Some(entry) = self.winner(Channel::Look) {
            status.has_look = true;
            status.look_x = entry.x;
            status.look_y = entry.y;
        }
        status
    }

    /// Highest-priority active source for a channel: live streaming sources
    /// first, then the latest update among sources of the same class.
    fn winner(&self, channel: Channel) -> Option<&Entry> {
        let mut winner: Option<(SourceId, &Entry)> = None;
        for (source, state) in &self.sources {
            let entry = channel.of(state);
            if !entry.active {
                continue;
            }
            let outranks = match winner {
                None => true,
                Some((best_source, best)) => match (source.streaming(), best_source.streaming()) {
                    (true, false) => true,
                    (false, true) => false,
                    _ => entry.order > best.order,
                },
            };
            if outranks {
                winner = Some((*source, entry));
            }
        }
        winner.map(|(_, entry)| entry)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn persistent() -> SourceId {
        SourceId::Persistent
    }

    fn conn(id: u64) -> SourceId {
        SourceId::Connection(id)
    }

    fn model(index: i32, token: &str) -> ModelIdentity {
        ModelIdentity::new(index, token)
    }

    fn arbiter() -> Arbiter {
        Arbiter::new(model(0, "model-1"))
    }

    fn mouth(arbiter: &mut Arbiter, source: SourceId, value: f32) {
        assert_eq!(arbiter.update(source, ChannelValue::Mouth(value)), Ok(()));
    }

    fn look(arbiter: &mut Arbiter, source: SourceId, x: f32, y: f32) {
        assert_eq!(arbiter.update(source, ChannelValue::Look(x, y)), Ok(()));
    }

    #[test]
    fn stream_outranks_persistent_even_when_persistent_is_newer() {
        let mut arbiter = arbiter();
        mouth(&mut arbiter, conn(1), 0.8);
        mouth(&mut arbiter, persistent(), 0.3);
        assert_eq!(arbiter.status().mouth, 0.8);
        look(&mut arbiter, conn(1), -0.5, 0.4);
        look(&mut arbiter, persistent(), 0.1, 0.2);
        let status = arbiter.status();
        assert_eq!((status.look_x, status.look_y), (-0.5, 0.4));
    }

    #[test]
    fn latest_update_wins_within_stream_class() {
        let mut arbiter = arbiter();
        mouth(&mut arbiter, conn(1), 0.2);
        mouth(&mut arbiter, conn(2), 0.9);
        assert_eq!(arbiter.status().mouth, 0.9);
        mouth(&mut arbiter, conn(1), 0.4);
        assert_eq!(arbiter.status().mouth, 0.4);
        look(&mut arbiter, conn(1), -0.5, 0.4);
        look(&mut arbiter, conn(2), 0.7, 0.6);
        assert_eq!(arbiter.status().look_x, 0.7);
        look(&mut arbiter, conn(1), 0.3, -0.2);
        assert_eq!(arbiter.status().look_x, 0.3);
    }

    #[test]
    fn latest_update_wins_within_persistent_class() {
        let mut arbiter = arbiter();
        mouth(&mut arbiter, persistent(), 0.2);
        mouth(&mut arbiter, persistent(), 0.6);
        assert_eq!(arbiter.status().mouth, 0.6);
        look(&mut arbiter, persistent(), 0.1, 0.2);
        look(&mut arbiter, persistent(), -0.1, -0.2);
        assert_eq!(arbiter.status().look_x, -0.1);
    }

    #[test]
    fn release_falls_back_to_previous_qualifying_source() {
        let mut arbiter = arbiter();
        look(&mut arbiter, persistent(), 0.1, 0.2);
        look(&mut arbiter, conn(1), -0.5, 0.4);
        look(&mut arbiter, conn(2), 0.7, 0.6);
        assert_eq!(arbiter.status().look_x, 0.7);
        arbiter.release(conn(2));
        assert_eq!(arbiter.status().look_x, -0.5);
        arbiter.release(conn(1));
        assert_eq!(arbiter.status().look_x, 0.1);
        arbiter.release(persistent());
        assert!(!arbiter.status().has_look);
    }

    #[test]
    fn disconnected_connection_releases_only_its_own_controls() {
        let mut arbiter = arbiter();
        mouth(&mut arbiter, persistent(), 0.3);
        look(&mut arbiter, persistent(), 0.1, 0.2);
        mouth(&mut arbiter, conn(1), 0.4);
        look(&mut arbiter, conn(1), 0.2, -0.3);
        mouth(&mut arbiter, conn(2), 0.9);
        arbiter.disconnect(1);
        let status = arbiter.status();
        assert_eq!(status.mouth, 0.9);
        assert_eq!((status.look_x, status.look_y), (0.1, 0.2));
        arbiter.disconnect(2);
        let status = arbiter.status();
        assert_eq!(status.mouth, 0.3);
        assert!(status.has_look);
    }

    #[test]
    fn disconnect_keeps_the_persistent_cli_source() {
        let mut arbiter = arbiter();
        mouth(&mut arbiter, persistent(), 0.3);
        look(&mut arbiter, persistent(), 0.1, 0.2);
        mouth(&mut arbiter, conn(7), 0.8);
        look(&mut arbiter, conn(7), -0.5, 0.4);
        arbiter.disconnect(7);
        let status = arbiter.status();
        assert_eq!((status.has_mouth, status.mouth), (true, 0.3));
        assert_eq!((status.has_look, status.look_x), (true, 0.1));
        arbiter.disconnect(7);
        arbiter.disconnect(8);
        assert_eq!(arbiter.status(), status);
    }

    #[test]
    fn connections_are_isolated() {
        let mut arbiter = arbiter();
        mouth(&mut arbiter, conn(1), 0.2);
        look(&mut arbiter, conn(1), 0.5, 0.5);
        mouth(&mut arbiter, conn(2), 0.6);
        look(&mut arbiter, conn(2), -0.5, -0.5);
        assert_eq!(arbiter.status().mouth, 0.6);
        assert_eq!(arbiter.status().look_x, -0.5);
        arbiter.reset(conn(2), Channel::Mouth);
        let status = arbiter.status();
        assert_eq!(status.mouth, 0.2);
        assert_eq!(status.look_x, -0.5);
        // A streaming write only creates the channel it names.
        look(&mut arbiter, conn(3), 0.25, 0.0);
        let status = arbiter.status();
        assert_eq!(status.look_x, 0.25);
        assert_eq!(status.mouth, 0.2);
    }

    #[test]
    fn reset_clears_one_channel_and_release_clears_both() {
        let mut arbiter = arbiter();
        mouth(&mut arbiter, conn(1), 0.8);
        look(&mut arbiter, conn(1), -0.5, 0.4);
        arbiter.reset(conn(1), Channel::Mouth);
        let status = arbiter.status();
        assert!(!status.has_mouth);
        assert_eq!((status.has_look, status.look_x), (true, -0.5));
        arbiter.reset(conn(1), Channel::Look);
        assert!(!arbiter.status().has_look);

        mouth(&mut arbiter, conn(1), 0.8);
        look(&mut arbiter, conn(1), -0.5, 0.4);
        arbiter.release(conn(1));
        let status = arbiter.status();
        assert!(!status.has_mouth && !status.has_look);
    }

    #[test]
    fn reset_and_release_are_scoped_to_the_named_source() {
        let mut arbiter = arbiter();
        mouth(&mut arbiter, persistent(), 0.3);
        look(&mut arbiter, persistent(), 0.1, 0.2);
        mouth(&mut arbiter, conn(1), 0.8);
        look(&mut arbiter, conn(1), -0.5, 0.4);
        // Resetting a source that never wrote has no effect.
        arbiter.reset(conn(9), Channel::Mouth);
        arbiter.release(conn(9));
        assert_eq!(arbiter.status().mouth, 0.8);
        arbiter.reset(conn(1), Channel::Mouth);
        assert_eq!(arbiter.status().mouth, 0.3);
        arbiter.release(conn(1));
        let status = arbiter.status();
        assert_eq!(status.mouth, 0.3);
        assert_eq!(status.look_x, 0.1);
    }

    #[test]
    fn release_source_affects_both_channels_of_a_source() {
        let mut arbiter = arbiter();
        mouth(&mut arbiter, persistent(), 0.3);
        look(&mut arbiter, persistent(), 0.1, 0.2);
        mouth(&mut arbiter, conn(2), 0.8);
        look(&mut arbiter, conn(2), -0.5, 0.4);
        arbiter.release(conn(2));
        let status = arbiter.status();
        assert_eq!((status.mouth, status.look_x), (0.3, 0.1));
        arbiter.release(persistent());
        assert_eq!(arbiter.status(), ControlStatus::default());
    }

    #[test]
    fn model_switch_clears_only_on_real_identity_change() {
        let mut arbiter = arbiter();
        mouth(&mut arbiter, persistent(), 0.3);
        look(&mut arbiter, conn(1), -0.5, 0.4);
        assert!(!arbiter.model_switch(model(0, "model-1")));
        assert!(arbiter.status().has_mouth && arbiter.status().has_look);
        // Re-selecting the current model is a successful no-op with a new token.
        assert!(arbiter.model_switch(model(1, "model-2")));
        assert_eq!(arbiter.status(), ControlStatus::default());
        assert_eq!(arbiter.model_token(), "model-2");
        assert!(!arbiter.model_switch(model(1, "model-2")));
    }

    #[test]
    fn model_switch_clears_when_only_the_token_changed() {
        let mut arbiter = arbiter();
        mouth(&mut arbiter, persistent(), 0.3);
        assert!(arbiter.model_switch(model(0, "model-2")));
        assert!(!arbiter.status().has_mouth);
    }

    #[test]
    fn failed_selection_keeps_sources() {
        let mut arbiter = arbiter();
        mouth(&mut arbiter, persistent(), 0.4);
        look(&mut arbiter, conn(1), 0.2, 0.3);
        // A failed selection never reports a new identity, so no switch happens.
        let status = arbiter.status();
        assert_eq!((status.mouth, status.look_x), (0.4, 0.2));
        assert_eq!(arbiter.model_token(), "model-1");
        assert!(arbiter.token_is_current("model-1"));
    }

    #[test]
    fn stale_model_token_is_rejected() {
        let mut arbiter = arbiter();
        assert!(arbiter.token_is_current("model-1"));
        assert!(!arbiter.token_is_current("model-0"));
        assert!(!arbiter.token_is_current(""));
        assert!(arbiter.model_switch(model(1, "model-2")));
        assert!(!arbiter.token_is_current("model-1"));
        assert!(arbiter.token_is_current("model-2"));
    }

    #[test]
    fn invalid_mouth_is_rejected_without_modifying_state() {
        let mut arbiter = arbiter();
        mouth(&mut arbiter, persistent(), 0.3);
        for value in [2.0, -0.1, f32::NAN, f32::INFINITY, f32::NEG_INFINITY] {
            assert_eq!(
                arbiter.update(conn(1), ChannelValue::Mouth(value)),
                Err(ControlError::MouthOutOfRange)
            );
        }
        let status = arbiter.status();
        assert_eq!((status.has_mouth, status.mouth), (true, 0.3));
        assert!(!arbiter.status().has_look);
        mouth(&mut arbiter, conn(1), 0.0);
        mouth(&mut arbiter, conn(1), 1.0);
        assert_eq!(arbiter.status().mouth, 1.0);
    }

    #[test]
    fn invalid_look_is_rejected_without_modifying_state() {
        let mut arbiter = arbiter();
        look(&mut arbiter, conn(1), 0.2, -0.3);
        for (x, y) in [
            (f32::INFINITY, 0.0),
            (0.0, f32::NEG_INFINITY),
            (f32::NAN, f32::NAN),
        ] {
            assert_eq!(
                arbiter.update(conn(1), ChannelValue::Look(x, y)),
                Err(ControlError::LookNotFinite)
            );
        }
        let status = arbiter.status();
        assert_eq!((status.look_x, status.look_y), (0.2, -0.3));
    }

    #[test]
    fn status_reports_default_values_when_nothing_is_active() {
        let arbiter = arbiter();
        assert_eq!(
            arbiter.status(),
            ControlStatus {
                has_mouth: false,
                mouth: 0.0,
                has_look: false,
                look_x: 0.0,
                look_y: 0.0,
            }
        );
    }

    /// Mirrors `plugins/desktop/tests/ExternalControlTest.cpp` step by step.
    #[test]
    fn external_control_flow_matches_the_cpp_reference() {
        let mut arbiter = arbiter();
        mouth(&mut arbiter, persistent(), 0.3);
        look(&mut arbiter, persistent(), 0.1, 0.2);
        let status = arbiter.status();
        assert_eq!((status.mouth, status.look_x), (0.3, 0.1));

        mouth(&mut arbiter, conn(2), 0.8);
        look(&mut arbiter, conn(3), -0.5, 0.4);
        look(&mut arbiter, conn(4), 0.7, 0.6);
        let status = arbiter.status();
        assert_eq!((status.mouth, status.look_x), (0.8, 0.7));

        arbiter.release(conn(4));
        let status = arbiter.status();
        assert_eq!((status.look_x, status.mouth), (-0.5, 0.8));

        // The persistent CLI value survives the streaming source's disconnect.
        arbiter.disconnect(2);
        let status = arbiter.status();
        assert_eq!((status.mouth, status.look_x), (0.3, -0.5));

        arbiter.reset(conn(3), Channel::Look);
        assert_eq!(arbiter.status().look_x, 0.1);

        arbiter.reset(persistent(), Channel::Mouth);
        let status = arbiter.status();
        assert!(!status.has_mouth && status.has_look);

        assert_eq!(
            arbiter.update(conn(5), ChannelValue::Look(f32::INFINITY, 0.0)),
            Err(ControlError::LookNotFinite)
        );
        assert_eq!(
            arbiter.update(conn(5), ChannelValue::Mouth(2.0)),
            Err(ControlError::MouthOutOfRange)
        );

        mouth(&mut arbiter, persistent(), 0.4);
        assert!(arbiter.model_switch(model(1, "model-2")));
        let status = arbiter.status();
        assert!(!status.has_mouth && !status.has_look);

        mouth(&mut arbiter, persistent(), 0.5);
        arbiter.release(persistent());
        assert!(!arbiter.status().has_mouth);
    }
}
