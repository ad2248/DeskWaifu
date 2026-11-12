//! Opt-in test against the real default output device.
//!
//! Enabled only with `DESKWAIFU_AUDIO_TEST_DEVICE=1`. Without it the test
//! reports that hardware verification was skipped and returns; it never prints
//! a passing assertion for a device it did not touch. This follows the repo
//! convention of `rust/cubism-core/tests/core.rs` and
//! `rust/cubism-render/tests/surfaceless.rs`: an unset variable means the
//! hardware path is unverified, not verified.
//!
//! The buffer is silent, so running this test is not audible.

use std::time::{Duration, Instant};

use deskwaifu_audio_output::{Event, Player, SourceFormat};

const RATE: u32 = 24_000;
const TIMEOUT: Duration = Duration::from_secs(5);

/// Half a second of 24 kHz mono silence, split into uneven chunks.
fn silence_chunks() -> Vec<Vec<u8>> {
    let total = RATE as usize / 2;
    let mut remaining = total;
    let mut chunks = Vec::new();
    while remaining > 0 {
        let size = (chunks.len() * 137 + 1).min(remaining);
        chunks.push(vec![0u8; size * 2]);
        remaining -= size;
    }
    chunks
}

#[test]
fn real_device_plays_silence_and_drains() {
    if std::env::var_os("DESKWAIFU_AUDIO_TEST_DEVICE").is_none() {
        eprintln!(
            "Skipping real audio device test: set DESKWAIFU_AUDIO_TEST_DEVICE=1 to play \
             half a second of silence on the default output. Hardware output is unverified."
        );
        return;
    }

    let mut player = Player::new();
    player.start(
        1,
        SourceFormat {
            sample_rate: RATE,
            channels: 1,
        },
    );
    for event in player.take_events() {
        assert!(
            matches!(event, Event::PlaybackStarted(1)),
            "the default device must accept 24 kHz mono: {event:?}"
        );
    }
    assert_eq!(player.id(), 1, "no usable default output device");

    let mut drained = false;
    let started = Instant::now();
    for chunk in silence_chunks() {
        player.append(1, &chunk);
        player.pump();
        for event in player.take_events() {
            match event {
                Event::Drained(1) => drained = true,
                Event::Failed(id, error) => panic!("playback {id} failed: {error:?}"),
                other => eprintln!("event: {other:?}"),
            }
        }
    }
    player.finish_input(1);

    while started.elapsed() < TIMEOUT {
        player.pump();
        for event in player.take_events() {
            match event {
                Event::Drained(1) => drained = true,
                Event::Failed(id, error) => panic!("playback {id} failed: {error:?}"),
                other => eprintln!("event: {other:?}"),
            }
        }
        if drained {
            break;
        }
        std::thread::sleep(Duration::from_millis(10));
    }

    assert!(
        drained,
        "the real device did not report Drained within {TIMEOUT:?}; \
         the output path was not verified"
    );
    assert_eq!(player.id(), 0);
    assert_eq!(player.pending_bytes(), 0);
}
