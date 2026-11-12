use deskwaifu_speech_domain::player::{FinishReason, Outcome, Player, PlayerError, State};
use deskwaifu_speech_domain::Error;
use std::collections::BTreeMap;

fn states(outcomes: &[Outcome]) -> Vec<State> {
    outcomes
        .iter()
        .filter_map(|outcome| match outcome {
            Outcome::StateChanged(state) => Some(*state),
            _ => None,
        })
        .collect()
}

fn finished(outcomes: &[Outcome]) -> Vec<(u64, FinishReason)> {
    outcomes
        .iter()
        .filter_map(|outcome| match outcome {
            Outcome::Finished { id, reason } => Some((*id, *reason)),
            _ => None,
        })
        .collect()
}

fn accepted(outcomes: &[Outcome]) -> Vec<u64> {
    outcomes
        .iter()
        .filter_map(|outcome| match outcome {
            Outcome::Accepted { id } => Some(*id),
            _ => None,
        })
        .collect()
}

/// Checks that every accepted id reached exactly one terminal outcome.
fn assert_single_terminal_per_id(outcomes: &[Outcome]) {
    let mut terminals: BTreeMap<u64, usize> = BTreeMap::new();
    for (id, _) in finished(outcomes) {
        *terminals.entry(id).or_default() += 1;
    }
    for (id, count) in terminals {
        assert_eq!(count, 1, "id {id} must finish exactly once");
    }
}

#[test]
fn new_player_is_idle_without_id() {
    let mut player = Player::new();
    assert_eq!(player.state(), State::Idle);
    assert_eq!(player.id(), 0);
    assert!(player.take_outcomes().is_empty());
}

#[test]
fn happy_path_completes_only_after_drain() {
    let mut player = Player::new();

    let id = player.speak("hello").expect("non-blank request accepted");
    assert_eq!(id, 1);
    assert_eq!(player.state(), State::Preparing);
    assert_eq!(
        player.take_outcomes(),
        vec![
            Outcome::Accepted { id },
            Outcome::StateChanged(State::Preparing),
        ]
    );

    assert!(player.ready(id));
    player.started(id);
    assert_eq!(player.state(), State::Playing);
    assert_eq!(
        player.take_outcomes(),
        vec![Outcome::StateChanged(State::Playing)]
    );

    player.completed(id);
    assert_eq!(player.state(), State::Draining);
    assert_eq!(
        player.take_outcomes(),
        vec![Outcome::StateChanged(State::Draining)]
    );

    player.drained(id);
    assert_eq!(player.state(), State::Idle);
    assert_eq!(player.id(), 0);
    assert_eq!(
        player.take_outcomes(),
        vec![
            Outcome::Finished {
                id,
                reason: FinishReason::Completed,
            },
            Outcome::StateChanged(State::Idle),
        ]
    );
}

#[test]
fn completed_without_playing_still_drains() {
    let mut player = Player::new();
    let id = player.speak("zero audio").expect("accepted");
    player.take_outcomes();

    player.completed(id);
    assert_eq!(player.state(), State::Draining);
    player.drained(id);
    assert_eq!(
        finished(&player.take_outcomes()),
        vec![(id, FinishReason::Completed)]
    );
}

#[test]
fn replace_mid_playback() {
    let mut player = Player::new();
    let old = player.speak("first").expect("accepted");
    player.started(old);
    player.take_outcomes();

    let new = player.speak("second").expect("accepted");
    assert!(new > old);
    assert_eq!(player.state(), State::Preparing);
    assert_eq!(player.id(), new);
    assert_eq!(
        player.take_outcomes(),
        vec![
            Outcome::Finished {
                id: old,
                reason: FinishReason::Replaced,
            },
            Outcome::Accepted { id: new },
            Outcome::StateChanged(State::Preparing),
        ]
    );
}

#[test]
fn replace_mid_preparing() {
    let mut player = Player::new();
    let old = player.speak("first").expect("accepted");
    player.take_outcomes();

    let new = player.speak("second").expect("accepted");
    assert_eq!(player.state(), State::Preparing);
    assert_eq!(
        player.take_outcomes(),
        vec![
            Outcome::Finished {
                id: old,
                reason: FinishReason::Replaced,
            },
            Outcome::Accepted { id: new },
            // Preparing -> Preparing is an external transition in Qt, so the
            // state is re-entered and reported again.
            Outcome::StateChanged(State::Preparing),
        ]
    );
}

#[test]
fn replace_mid_draining_finishes_old_as_replaced() {
    let mut player = Player::new();
    let old = player.speak("first").expect("accepted");
    player.started(old);
    player.completed(old);
    player.take_outcomes();

    let new = player.speak("second").expect("accepted");
    assert_eq!(
        finished(&player.take_outcomes()),
        vec![(old, FinishReason::Replaced)]
    );
    // The old drain never becomes Completed.
    player.drained(old);
    assert_eq!(player.id(), new);
    assert_eq!(player.state(), State::Preparing);
    assert!(player.take_outcomes().is_empty());
}

#[test]
fn stop_from_each_active_state() {
    for start in [State::Preparing, State::Playing, State::Draining] {
        let mut player = Player::new();
        let id = player.speak("text").expect("accepted");
        match start {
            State::Preparing => {}
            State::Playing => player.started(id),
            State::Draining => {
                player.started(id);
                player.completed(id);
            }
            State::Idle => unreachable!(),
        }
        assert_eq!(player.state(), start);
        player.take_outcomes();

        player.stop();
        assert_eq!(player.state(), State::Idle);
        assert_eq!(player.id(), 0);
        assert_eq!(
            player.take_outcomes(),
            vec![
                Outcome::Finished {
                    id,
                    reason: FinishReason::Stopped,
                },
                Outcome::StateChanged(State::Idle),
            ]
        );
    }
}

#[test]
fn stop_while_idle_is_a_noop() {
    let mut player = Player::new();
    player.take_outcomes();

    player.stop();
    assert_eq!(player.state(), State::Idle);
    assert_eq!(player.id(), 0);
    assert!(player.take_outcomes().is_empty());
}

#[test]
fn blank_text_does_not_interrupt_or_allocate() {
    let mut player = Player::new();
    let id = player.speak("active").expect("accepted");
    player.take_outcomes();

    for blank in [
        "",
        " ",
        "\t",
        "\n",
        " \t\n ",
        "\u{00a0}\u{3000}",
        "\u{2003}",
    ] {
        assert_eq!(player.speak(blank), None, "{blank:?} must be blank");
        assert_eq!(player.state(), State::Preparing);
        assert_eq!(player.id(), id);
        assert!(player.take_outcomes().is_empty());
    }

    // Next accepted request still gets id 2: no id was burnt by blanks.
    assert_eq!(player.speak("next"), Some(id + 1));
}

#[test]
fn stale_events_are_ignored() {
    let mut player = Player::new();
    let old = player.speak("first").expect("accepted");
    player.started(old);
    let new = player.speak("second").expect("accepted");
    player.take_outcomes();

    // Every event of the replaced id, in every order, changes nothing.
    assert!(!player.ready(old));
    player.started(old);
    player.completed(old);
    player.drained(old);
    player.failed(old, PlayerError::Source(Error::Network));
    player.started(new); // advance current to Playing
    player.completed(new); // and to Draining
    player.take_outcomes();

    assert!(!player.ready(old));
    player.started(old);
    player.completed(old);
    player.drained(old);
    player.failed(old, PlayerError::Source(Error::Network));
    assert_eq!(player.state(), State::Draining);
    assert_eq!(player.id(), new);
    assert!(player.take_outcomes().is_empty());
}

#[test]
fn stale_drained_cannot_complete_new_playback() {
    let mut player = Player::new();
    let old = player.speak("first").expect("accepted");
    player.started(old);
    player.completed(old);
    assert_eq!(player.state(), State::Draining);

    let new = player.speak("second").expect("accepted");
    player.take_outcomes();

    player.drained(old);
    assert_eq!(
        player.state(),
        State::Preparing,
        "stale drain must not finish the new request"
    );
    assert_eq!(player.id(), new);
    assert!(player.take_outcomes().is_empty());

    player.drained(new);
    assert_eq!(
        player.state(),
        State::Preparing,
        "drain outside Draining is ignored"
    );
    assert!(player.take_outcomes().is_empty());
}

#[test]
fn zero_id_events_are_ignored() {
    let mut player = Player::new();
    let id = player.speak("text").expect("accepted");
    player.take_outcomes();

    assert!(!player.ready(0));
    player.started(0);
    player.completed(0);
    player.drained(0);
    player.failed(0, PlayerError::AudioDevice);
    assert_eq!(player.state(), State::Preparing);
    assert_eq!(player.id(), id);
    assert!(player.take_outcomes().is_empty());
}

#[test]
fn failure_on_active_id_emits_error_once_and_fails() {
    let mut player = Player::new();
    let id = player.speak("text").expect("accepted");
    player.take_outcomes();

    player.failed(id, PlayerError::Source(Error::Protocol));
    assert_eq!(player.state(), State::Idle);
    assert_eq!(player.id(), 0);
    let outcomes = player.take_outcomes();
    assert_eq!(
        outcomes[0],
        Outcome::Error {
            id,
            error: PlayerError::Source(Error::Protocol),
        }
    );
    assert_eq!(finished(&outcomes), vec![(id, FinishReason::Failed)]);
    assert_eq!(states(&outcomes), vec![State::Idle]);

    // A second failure for the same, now stale, id is ignored.
    player.failed(id, PlayerError::AudioDevice);
    assert!(player.take_outcomes().is_empty());
}

#[test]
fn failure_from_each_active_state() {
    for start in [State::Preparing, State::Playing, State::Draining] {
        let mut player = Player::new();
        let id = player.speak("text").expect("accepted");
        match start {
            State::Preparing => {}
            State::Playing => player.started(id),
            State::Draining => {
                player.started(id);
                player.completed(id);
            }
            State::Idle => unreachable!(),
        }
        player.take_outcomes();

        player.failed(id, PlayerError::Source(Error::Timeout));
        assert_eq!(player.state(), State::Idle);
        assert_eq!(
            finished(&player.take_outcomes()),
            vec![(id, FinishReason::Failed)]
        );
    }
}

#[test]
fn recovery_after_failure_allocates_new_id() {
    let mut player = Player::new();
    let id = player.speak("test").expect("accepted");
    player.failed(id, PlayerError::Source(Error::Synthesis(1004)));
    assert_eq!(player.state(), State::Idle);
    player.take_outcomes();

    let next = player.speak("again").expect("accepted");
    assert!(next > id);
    player.started(next);
    player.completed(next);
    player.drained(next);
    assert_eq!(
        finished(&player.take_outcomes()),
        vec![(next, FinishReason::Completed)]
    );
}

#[test]
fn ids_are_monotonic_across_replacements_and_restarts() {
    let mut player = Player::new();
    let mut ids = Vec::new();
    for _ in 0..5 {
        ids.push(player.speak("text").expect("accepted"));
        ids.push(player.speak("replacement").expect("accepted"));
    }
    player.stop();
    ids.push(player.speak("after stop").expect("accepted"));
    player.failed(ids.last().copied().unwrap(), PlayerError::AudioDevice);
    ids.push(player.speak("after failure").expect("accepted"));
    player.started(ids.last().copied().unwrap());
    player.completed(ids.last().copied().unwrap());
    player.drained(ids.last().copied().unwrap());

    assert_eq!(ids, (1..=ids.len() as u64).collect::<Vec<_>>());
    assert_single_terminal_per_id(&player.take_outcomes());
}

#[test]
fn terminal_outcomes_are_never_duplicated() {
    let mut player = Player::new();
    let mut all = Vec::new();
    let drain = |player: &mut Player, all: &mut Vec<Outcome>| {
        all.extend(player.take_outcomes());
    };

    // Interleave every event kind with every state, then run to quiescence.
    let first = player.speak("first").expect("accepted");
    drain(&mut player, &mut all);
    player.started(first);
    player.completed(first);
    player.drained(first);
    drain(&mut player, &mut all);

    let second = player.speak("second").expect("accepted");
    drain(&mut player, &mut all);
    player.completed(second);
    player.drained(second);
    drain(&mut player, &mut all);

    let third = player.speak("third").expect("accepted");
    drain(&mut player, &mut all);
    player.stop();
    drain(&mut player, &mut all);

    let fourth = player.speak("fourth").expect("accepted");
    drain(&mut player, &mut all);
    player.failed(fourth, PlayerError::Source(Error::Protocol));
    drain(&mut player, &mut all);

    // Quiescent: nothing is pending, further input only allocates when spoken.
    player.stop();
    player.started(fourth);
    player.completed(fourth);
    player.drained(fourth);
    player.failed(fourth, PlayerError::AudioDevice);
    drain(&mut player, &mut all);

    assert_eq!(accepted(&all), vec![first, second, third, fourth]);
    assert_eq!(
        finished(&all),
        vec![
            (first, FinishReason::Completed),
            (second, FinishReason::Completed),
            (third, FinishReason::Stopped),
            (fourth, FinishReason::Failed),
        ]
    );
    assert_single_terminal_per_id(&all);
    assert_eq!(player.state(), State::Idle);
    assert_eq!(player.id(), 0);

    let terminal_count = all
        .iter()
        .filter(|outcome| matches!(outcome, Outcome::Finished { .. }))
        .count();
    assert_eq!(terminal_count, accepted(&all).len());
}
