//! Playback contract tests over an injectable device.
//!
//! `Fake` stands in for cpal: it owns the queue and progress counters and lets
//! the test decide when the "device" asks for another callback buffer. Every
//! test drives the same queue, conversion and drain logic the real sink uses;
//! only the hardware is replaced.

use std::sync::{Arc, Mutex, MutexGuard};

use deskwaifu_audio_output::{
    Converter, DeviceFormat, Error, ErrorCode, Event, Player, Progress, SampleFormat, Shared, Sink,
    SinkFactory, SourceFormat, MAX_PENDING_BYTES,
};

const MONO_24K: SourceFormat = SourceFormat {
    sample_rate: 24_000,
    channels: 1,
};

fn device_format() -> DeviceFormat {
    DeviceFormat {
        sample_rate: 24_000,
        channels: 2,
        sample_format: SampleFormat::F32,
    }
}

#[derive(Default)]
struct State {
    source: Option<SourceFormat>,
    queue: Option<Shared>,
    converter: Option<Converter>,
    progress: Option<Arc<Progress>>,
    opened: usize,
    dropped: usize,
    fail_open: bool,
}

#[derive(Clone)]
struct Fake(Arc<Mutex<State>>);

impl Fake {
    fn new() -> Self {
        Self(Arc::new(Mutex::new(State::default())))
    }

    fn failing() -> Self {
        let fake = Self::new();
        Self::state(&fake).fail_open = true;
        fake
    }

    fn state(&self) -> MutexGuard<'_, State> {
        self.0.lock().unwrap_or_else(|error| error.into_inner())
    }

    /// Emulates one device callback of `frames` frames and returns the number
    /// of frames that received queued audio.
    fn pull(&self, frames: usize) -> usize {
        self.pull_at(frames, 0, 0)
    }

    fn pull_at(&self, frames: usize, callback_ns: u64, playback_ns: u64) -> usize {
        let state = self.state();
        let (Some(queue), Some(converter), Some(progress)) =
            (state.queue.clone(), state.converter, state.progress.clone())
        else {
            return 0;
        };
        let device = converter.device();
        let mut buffer = vec![0u8; frames * device.frame_bytes()];
        if callback_ns == 0 {
            progress.begin_callback();
        } else {
            progress.begin_callback_at(callback_ns);
        }
        progress.begin_fill();
        let filled = deskwaifu_audio_output::Queue::fill(
            &mut queue.lock().unwrap_or_else(|error| error.into_inner()),
            &converter,
            &mut buffer,
        );
        if playback_ns == 0 {
            progress.delivered(filled as u64);
        } else {
            progress.delivered_at(filled as u64, playback_ns, MONO_24K.sample_rate);
        }
        progress.end_fill();
        filled
    }

    /// Emulates a callback that has already taken the last queued audio but has
    /// not published its frame count yet, which is the window the drain check
    /// must not mistake for completion.
    fn pull_and_hold(&self, frames: usize) -> HeldFill {
        let state = self.state();
        let (Some(queue), Some(converter), Some(progress)) =
            (state.queue.clone(), state.converter, state.progress.clone())
        else {
            panic!("open stream");
        };
        let device = converter.device();
        let mut buffer = vec![0u8; frames * device.frame_bytes()];
        progress.begin_callback();
        progress.begin_fill();
        let filled = deskwaifu_audio_output::Queue::fill(
            &mut queue.lock().unwrap_or_else(|error| error.into_inner()),
            &converter,
            &mut buffer,
        );
        drop(state);
        HeldFill {
            progress,
            filled,
            done: false,
        }
    }

    /// Emulates a device callback that reports a stream error.
    fn report_error(&self) {
        self.state().progress.as_ref().expect("open stream").fail();
    }

    fn delivered(&self) -> u64 {
        self.state()
            .progress
            .as_ref()
            .map_or(0, |progress| progress.delivered_frames())
    }
}

impl SinkFactory for Fake {
    fn open(&mut self, source: SourceFormat, queue: Shared) -> Result<Box<dyn Sink>, Error> {
        let mut state = self.state();
        state.opened += 1;
        if state.fail_open {
            return Err(Error::START);
        }
        let converter = Converter::new(source, device_format()).expect("fake device format");
        let progress = Arc::new(Progress::new());
        state.source = Some(source);
        state.queue = Some(queue);
        state.converter = Some(converter);
        state.progress = Some(Arc::clone(&progress));
        drop(state);
        Ok(Box::new(FakeSink {
            owner: self.clone(),
            progress,
        }))
    }
}

struct FakeSink {
    owner: Fake,
    progress: Arc<Progress>,
}

/// A callback that took audio out of the queue but has not published it yet.
struct HeldFill {
    progress: Arc<Progress>,
    filled: usize,
    done: bool,
}

impl HeldFill {
    /// Publishes the frame count, finishing the callback.
    fn release(mut self) -> usize {
        self.progress.delivered(self.filled as u64);
        self.progress.end_fill();
        self.done = true;
        self.filled
    }
}

impl Drop for HeldFill {
    fn drop(&mut self) {
        if !self.done {
            self.progress.end_fill();
        }
    }
}

impl Drop for FakeSink {
    fn drop(&mut self) {
        self.owner.state().dropped += 1;
    }
}

impl Sink for FakeSink {
    fn format(&self) -> DeviceFormat {
        device_format()
    }

    fn progress(&self) -> Arc<Progress> {
        Arc::clone(&self.progress)
    }
}

fn player() -> (Player<Fake>, Fake) {
    let fake = Fake::new();
    (Player::with_factory(fake.clone()), fake)
}

/// One full idle period: the callback that drained the queue, then the next
/// one that reports the device buffer empty.
fn drain(fake: &Fake, player: &mut Player<Fake>) {
    fake.pull(256);
    player.pump();
    fake.pull(256);
    player.pump();
}

fn pcm(frames: usize) -> Vec<u8> {
    vec![0u8; frames * MONO_24K.frame_bytes()]
}

#[test]
fn append_before_start_is_rejected() {
    let (mut player, fake) = player();
    player.append(1, &pcm(8));
    player.finish_input(1);
    player.pump();
    assert!(player.take_events().is_empty());
    assert_eq!(player.id(), 0);
    assert_eq!(player.pending_bytes(), 0);
    assert_eq!(fake.state().opened, 0);
}

#[test]
fn append_after_finish_input_is_rejected() {
    let (mut player, fake) = player();
    player.start(1, MONO_24K);
    player.append(1, &pcm(4));
    player.finish_input(1);
    player.append(1, &pcm(4));
    assert_eq!(player.pending_bytes(), 4 * 2);
    drain(&fake, &mut player);
    assert_eq!(
        player.take_events(),
        vec![Event::PlaybackStarted(1), Event::Drained(1)]
    );
    assert_eq!(fake.delivered(), 4);
}

#[test]
fn every_operation_ignores_a_stale_id() {
    let (mut player, fake) = player();
    player.start(1, MONO_24K);
    player.start(2, MONO_24K);
    assert_eq!(fake.state().opened, 2);
    assert_eq!(fake.state().dropped, 1, "start discards the old stream");
    player.take_events();

    // Everything aimed at the replaced id is ignored completely.
    player.append(1, &pcm(8));
    assert_eq!(player.pending_bytes(), 0);
    player.finish_input(1);
    assert!(!player.input_finished());
    player.stop(1);
    assert_eq!(player.id(), 2);
    player.pump();
    assert!(
        player.take_events().is_empty(),
        "no device callback yet, so playback has not started"
    );
    fake.pull(256);
    player.pump();
    assert_eq!(player.take_events(), vec![Event::PlaybackStarted(2)]);

    // The active id still works.
    player.append(2, &pcm(4));
    player.finish_input(2);
    drain(&fake, &mut player);
    assert_eq!(player.take_events(), vec![Event::Drained(2)]);
}

#[test]
fn stop_discards_queued_audio_and_never_drains() {
    let (mut player, fake) = player();
    player.start(1, MONO_24K);
    player.append(1, &pcm(24_000));
    player.finish_input(1);
    player.stop(1);
    assert_eq!(player.id(), 0);
    assert_eq!(player.pending_bytes(), 0);
    assert_eq!(fake.state().dropped, 1);
    assert_eq!(fake.delivered(), 0, "nothing reached the device");

    // Stopping twice and pumping afterwards emit nothing.
    player.stop(1);
    player.pump();
    assert!(player.take_events().is_empty());
}

#[test]
fn drained_requires_finish_input_and_full_device_consumption() {
    let (mut player, fake) = player();
    player.start(1, MONO_24K);
    player.append(1, &pcm(512));

    // Playback runs while input is still open.
    assert_eq!(fake.pull(256), 256);
    player.pump();
    assert_eq!(player.take_events(), vec![Event::PlaybackStarted(1)]);

    // Queue empties but the source has not finished: never Drained.
    assert_eq!(fake.pull(256), 256);
    player.pump();
    fake.pull(256);
    player.pump();
    assert!(player.take_events().is_empty());

    // End of input with an empty queue and an idle device drains on the pump
    // that `finish_input` is followed by, like the C++ `Pump` call.
    player.finish_input(1);
    player.pump();
    assert_eq!(player.take_events(), vec![Event::Drained(1)]);
    assert_eq!(player.id(), 0);
    assert_eq!(player.pending_bytes(), 0);
}

#[test]
fn drained_waits_for_delivery_and_an_idle_callback() {
    let (mut player, fake) = player();
    player.start(1, MONO_24K);
    player.append(1, &pcm(200));
    player.finish_input(1);
    player.pump();
    assert!(
        player.take_events().is_empty(),
        "no device callback yet, so playback has not started"
    );

    // One callback delivers every queued frame; the device has not consumed
    // them yet, so this must not be reported as drained.
    assert_eq!(fake.pull(256), 200);
    player.pump();
    assert_eq!(
        player.take_events(),
        vec![Event::PlaybackStarted(1)],
        "delivered is not consumed"
    );
    assert_eq!(player.id(), 1);

    // The next callback means the previous buffer was consumed: now idle.
    fake.pull(256);
    player.pump();
    assert_eq!(player.take_events(), vec![Event::Drained(1)]);
}

#[test]
fn player_waits_for_timestamped_tail_audio_to_finish_playing() {
    let (mut player, fake) = player();
    player.start(1, MONO_24K);
    player.append(1, &pcm(240));
    player.finish_input(1);

    // The queue is emptied by the first callback; playback is predicted to
    // start at 10 ms and last another 10 ms (24 kHz).
    assert_eq!(fake.pull_at(256, 1_000_000, 10_000_000), 240);
    player.pump();
    assert_eq!(player.take_events(), vec![Event::PlaybackStarted(1)]);
    assert_eq!(player.pending_bytes(), 0);
    assert!(player.take_events().is_empty());

    // Empty/silent callback at 15 ms is not proof that the tail played.
    fake.pull_at(256, 15_000_000, 20_000_000);
    player.pump();
    assert!(player.take_events().is_empty());
    assert_eq!(player.id(), 1);

    // At the predicted end instant, the next callback advances progress and
    // Player can emit Drained.
    fake.pull_at(256, 20_000_000, 30_000_000);
    player.pump();
    assert_eq!(player.take_events(), vec![Event::Drained(1)]);
}

#[test]
fn drained_waits_while_a_callback_is_still_publishing_its_frames() {
    // Regression: the queue can look empty while the callback that emptied it
    // has not published its frame count yet. Reporting `Drained` there truncates
    // the last buffer.
    let (mut player, fake) = player();
    player.start(1, MONO_24K);
    player.append(1, &pcm(400));
    player.finish_input(1);
    assert_eq!(fake.pull(256), 256);
    player.pump();
    assert_eq!(player.take_events(), vec![Event::PlaybackStarted(1)]);

    // Take the remaining audio out of the queue but hold the publication.
    let held = fake.pull_and_hold(256);
    assert_eq!(player.pending_bytes(), 0, "queue is empty");
    player.pump();
    assert_eq!(
        player.take_events(),
        Vec::new(),
        "an in-flight callback must not count as drained"
    );
    assert_eq!(player.id(), 1, "playback is still open");

    // Publishing and then one idle callback finally drains it.
    held.release();
    fake.pull(256);
    player.pump();
    assert_eq!(player.take_events(), vec![Event::Drained(1)]);
}

#[test]
fn finish_input_with_queued_audio_waits_for_the_device() {
    let (mut player, fake) = player();
    player.start(1, MONO_24K);
    player.append(1, &pcm(400));
    player.finish_input(1);
    // 400 frames at 24 kHz is ~17 ms; nothing has reached the device yet.
    player.pump();
    assert!(
        player.take_events().is_empty(),
        "no device callback yet, so playback has not started"
    );
    assert_eq!(player.id(), 1, "still playing");

    assert_eq!(fake.pull(2048), 400, "all queued frames delivered");
    player.pump();
    assert_eq!(
        player.take_events(),
        vec![Event::PlaybackStarted(1)],
        "delivered but not yet consumed"
    );
    assert_eq!(player.id(), 1, "still playing");

    // The next callback consumed the previous buffer.
    assert_eq!(fake.pull(2048), 0);
    player.pump();
    assert_eq!(player.take_events(), vec![Event::Drained(1)]);
    assert_eq!(fake.delivered(), 400);
}

#[test]
fn drained_waits_for_audio_appended_after_finish_ordering() {
    // FinishInput sees half a frame and fails with Protocol, like the C++
    // `_pending.size() % bytesPerFrame` guard.
    let (mut player, _fake) = player();
    player.start(1, MONO_24K);
    player.append(1, &[0]);
    player.finish_input(1);
    assert_eq!(
        player.take_events(),
        vec![Event::Failed(1, Error::INCOMPLETE_FRAME)]
    );
    assert_eq!(Error::INCOMPLETE_FRAME.code, ErrorCode::Protocol);
    assert_eq!(player.id(), 0);
}

#[test]
fn empty_queue_with_end_of_input_drains_after_one_idle_period() {
    let (mut player, fake) = player();
    player.start(3, MONO_24K);
    player.finish_input(3);
    // Zero audio data still completes normally (C++ `stopAndReplacement...`).
    fake.pull(256);
    player.pump();
    fake.pull(256);
    player.pump();
    assert_eq!(
        player.take_events(),
        vec![Event::PlaybackStarted(3), Event::Drained(3)]
    );
}

#[test]
fn drained_is_never_reported_before_finish_input() {
    let (mut player, fake) = player();
    player.start(1, MONO_24K);
    player.append(1, &pcm(64));
    // Play the whole queue and reach device idle while input is still open.
    fake.pull(256);
    player.pump();
    fake.pull(256);
    player.pump();
    fake.pull(256);
    player.pump();
    assert_eq!(
        player.take_events(),
        vec![Event::PlaybackStarted(1)],
        "PlaybackStarted is not Drained"
    );
    assert_eq!(player.id(), 1, "an empty queue is not end of input");
    assert_eq!(player.pending_bytes(), 0);

    player.finish_input(1);
    player.pump();
    assert_eq!(player.take_events(), vec![Event::Drained(1)]);
}

#[test]
fn zero_length_append_changes_nothing() {
    let (mut player, fake) = player();
    player.start(1, MONO_24K);
    player.append(1, &[]);
    assert_eq!(player.pending_bytes(), 0);
    player.append(1, &pcm(2));
    player.append(1, &[]);
    assert_eq!(player.pending_bytes(), 4);
    player.finish_input(1);
    drain(&fake, &mut player);
    assert_eq!(
        player.take_events(),
        vec![Event::PlaybackStarted(1), Event::Drained(1)]
    );
}

#[test]
fn overflow_fails_at_one_byte_past_the_limit_and_accepts_the_limit() {
    let (mut player, _fake) = player();
    player.start(1, MONO_24K);
    player.append(1, &vec![0; MAX_PENDING_BYTES]);
    assert_eq!(player.pending_bytes(), MAX_PENDING_BYTES);
    assert!(player.take_events().is_empty(), "8 MiB is still accepted");
    player.append(1, &[0]);
    assert_eq!(
        player.take_events(),
        vec![Event::Failed(1, Error::OVERFLOW)]
    );
    assert_eq!(Error::OVERFLOW.code, ErrorCode::BufferOverflow);
    assert_eq!(player.id(), 0);
    assert_eq!(player.pending_bytes(), 0);
}

#[test]
fn overflow_in_a_single_append_matches_the_cpp_test() {
    let (mut player, _fake) = player();
    player.start(5, MONO_24K);
    player.append(5, &vec![0; MAX_PENDING_BYTES + 1]);
    assert_eq!(
        player.take_events(),
        vec![Event::Failed(5, Error::OVERFLOW)]
    );
}

#[test]
fn a_start_stop_start_cycle_uses_a_fresh_id() {
    let (mut player, fake) = player();
    player.start(1, MONO_24K);
    player.append(1, &pcm(100));
    player.stop(1);

    player.start(2, MONO_24K);
    assert_eq!(fake.state().opened, 2);
    assert_eq!(fake.state().dropped, 1);
    player.append(2, &pcm(8));
    player.finish_input(2);
    drain(&fake, &mut player);
    assert_eq!(
        player.take_events(),
        vec![Event::PlaybackStarted(2), Event::Drained(2)]
    );
    // Re-using the old id now that a new one is active is ignored as well.
    player.append(1, &pcm(8));
    player.finish_input(1);
    player.stop(1);
    assert_eq!(just_started(&mut player), None);
}

/// Starts id 9 and returns the initial event, for tests that only care that a
/// call did nothing.
fn just_started(player: &mut Player<Fake>) -> Option<Event> {
    player.start(9, MONO_24K);
    player.take_events().first().copied()
}

#[test]
fn a_failed_start_reports_audio_device() {
    let fake = Fake::failing();
    let mut player = Player::with_factory(fake.clone());
    player.start(4, MONO_24K);
    assert_eq!(player.take_events(), vec![Event::Failed(4, Error::START)]);
    assert_eq!(Error::START.code, ErrorCode::AudioDevice);
    assert_eq!(player.id(), 0);
    // Nothing is buffered or drained after the failure.
    player.append(4, &pcm(4));
    player.finish_input(4);
    player.pump();
    assert!(player.take_events().is_empty());
    assert_eq!(fake.delivered(), 0);
}

#[test]
fn a_stream_error_after_start_fails_once() {
    let (mut player, fake) = player();
    player.start(1, MONO_24K);
    player.append(1, &pcm(256));
    player.pump();
    player.take_events();
    fake.report_error();
    player.pump();
    assert_eq!(player.take_events(), vec![Event::Failed(1, Error::DEVICE)]);
    assert_eq!(Error::DEVICE.code, ErrorCode::AudioDevice);
    assert_eq!(player.id(), 0);
    player.pump();
    assert!(player.take_events().is_empty(), "failure is reported once");
}

#[test]
fn conversion_reaches_the_device_in_its_own_format() {
    let (mut player, fake) = player();
    player.start(1, MONO_24K);
    // 0.5 and -0.5 in mono PCM16.
    player.append(1, &[0x00, 0x40, 0x00, 0xc0]);
    player.finish_input(1);
    assert_eq!(fake.pull(4), 2);
    let state = fake.state();
    let progress = state.progress.clone().expect("stream");
    assert_eq!(progress.delivered_frames(), 2);
    assert!(state.converter.is_some());
    assert_eq!(
        state.converter.unwrap().device(),
        device_format(),
        "the device keeps its own f32 stereo format"
    );
    drop(state);
    fake.pull(4);
    player.pump();
    assert_eq!(
        player.take_events(),
        vec![Event::PlaybackStarted(1), Event::Drained(1)]
    );
}

#[test]
fn the_device_is_only_opened_once_per_start() {
    let (mut player, fake) = player();
    player.start(1, MONO_24K);
    for _ in 0..10 {
        player.append(1, &pcm(16));
        fake.pull(16);
        player.pump();
    }
    assert_eq!(fake.state().opened, 1);
    assert_eq!(player.pending_bytes(), 0);
}

#[test]
fn progress_counters_stay_consistent_under_partial_pulls() {
    let (mut player, fake) = player();
    player.start(1, MONO_24K);
    player.append(1, &pcm(10));
    assert_eq!(fake.pull(3), 3);
    assert_eq!(fake.pull(3), 3);
    assert_eq!(fake.pull(4), 4);
    assert_eq!(fake.pull(1), 0, "queue is empty, only silence is written");
    assert_eq!(fake.delivered(), 10);
    player.finish_input(1);
    player.pump();
    let events = player.take_events();
    assert!(events.contains(&Event::Drained(1)), "{events:?}");
    let state = fake.state();
    let progress = state.progress.clone().expect("stream");
    assert!(progress.played_frames() >= progress.delivered_frames());
    assert_eq!(progress.delivered_frames(), 10);
}
