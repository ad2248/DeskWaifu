//! Playback state machine and outcome queue.
//!
//! Port of `AudioPlayer.cpp`: same ids, same guards, same terminal events. The
//! only structural difference is that feeding the device happens inside the
//! audio callback instead of a 10 ms `QTimer`, so the caller drains the
//! outcomes with [`Player::pump`] and [`Player::take_events`].

use std::sync::Arc;

use crate::queue::{self, Shared};
use crate::sink::{CpalFactory, Progress, Sink, SinkFactory};
use crate::{Error, SourceFormat};

/// Outcome of one playback, mirroring the C++ signals.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Event {
    /// `PlaybackStarted(SpeechId)`: the output stream is running.
    PlaybackStarted(u64),
    /// `Drained(SpeechId)`: end of input arrived, every queued frame was
    /// delivered and the device finished playing it. Always the last event of
    /// a successful playback.
    Drained(u64),
    /// `Failed(SpeechId, SpeechError)`: the playback was aborted and cleaned up.
    Failed(u64, Error),
}

/// PCM output for one playback id at a time.
///
/// Every method ignores an id that is not current, including a stale id whose
/// playback already ended, exactly like the C++ guards `!id || id != _id`.
pub struct Player<F: SinkFactory = CpalFactory> {
    factory: F,
    id: u64,
    source: SourceFormat,
    queue: Option<Shared>,
    sink: Option<Box<dyn Sink>>,
    progress: Option<Arc<Progress>>,
    finished: bool,
    started: bool,
    events: Vec<Event>,
}

impl Default for Player<CpalFactory> {
    fn default() -> Self {
        Self::new()
    }
}

impl Player<CpalFactory> {
    /// Creates a player that opens the default output device.
    pub fn new() -> Self {
        Self::with_factory(CpalFactory)
    }
}

impl std::fmt::Debug for Player<CpalFactory> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Player")
            .field("id", &self.id)
            .field("source", &self.source)
            .field("finished", &self.finished)
            .finish_non_exhaustive()
    }
}

impl<F: SinkFactory> Player<F> {
    /// Creates a player over `factory`; tests inject a fake device here.
    pub fn with_factory(factory: F) -> Self {
        Self {
            factory,
            id: 0,
            source: SourceFormat {
                sample_rate: 0,
                channels: 0,
            },
            queue: None,
            sink: None,
            progress: None,
            finished: false,
            started: false,
            events: Vec::new(),
        }
    }

    /// Current playback id, 0 when idle. Ids are chosen by the caller.
    pub const fn id(&self) -> u64 {
        self.id
    }

    /// Whether [`Player::finish_input`] already arrived for the current id.
    pub const fn input_finished(&self) -> bool {
        self.finished
    }

    /// Pending source bytes not yet handed to the device.
    pub fn pending_bytes(&self) -> usize {
        self.queue
            .as_ref()
            .map_or(0, |queue| queue::lock(queue).len())
    }

    /// Removes and returns the events emitted since the last call.
    pub fn take_events(&mut self) -> Vec<Event> {
        std::mem::take(&mut self.events)
    }

    /// Cleans up any previous playback and opens the default device for `id`.
    ///
    /// Fails with [`Error::NO_DEVICE`] when no device accepts the source sample
    /// rate. The device's own channel count and sample format are used; the
    /// queued PCM is converted to them. Id 0 opens nothing.
    pub fn start(&mut self, id: u64, source: SourceFormat) {
        self.stop(self.id);
        self.id = id;
        self.source = source;
        if id == 0 {
            return;
        }
        if !source.is_valid() {
            self.fail(Error::NO_DEVICE);
            return;
        }
        let queue = queue::shared();
        match self.factory.open(source, Arc::clone(&queue)) {
            Ok(sink) => {
                self.progress = Some(sink.progress());
                self.sink = Some(sink);
                self.queue = Some(queue);
            }
            Err(error) => self.fail(error),
        }
    }

    /// Queues `pcm` for the current id.
    ///
    /// Ignored for a stale id, for id 0 and after end of input. Exceeding
    /// [`crate::MAX_PENDING_BYTES`] emits [`Error::OVERFLOW`] and stops the
    /// playback, as the C++ `Append` did.
    pub fn append(&mut self, id: u64, pcm: &[u8]) {
        if id == 0 || id != self.id || self.finished {
            return;
        }
        let Some(queue) = self.queue.clone() else {
            return;
        };
        let queued = queue::lock(&queue).append(pcm);
        if let Err(error) = queued {
            self.fail(error);
        }
    }

    /// Marks end of input; audio already queued keeps playing.
    ///
    /// A pending length that is not a whole number of source frames fails with
    /// [`Error::INCOMPLETE_FRAME`], matching `SpeechErrorCode::Protocol`.
    pub fn finish_input(&mut self, id: u64) {
        if id == 0 || id != self.id {
            return;
        }
        let Some(queue) = self.queue.clone() else {
            return;
        };
        let frame_bytes = self.source.frame_bytes();
        let finished = queue::lock(&queue).finish(frame_bytes);
        match finished {
            Ok(()) => self.finished = true,
            Err(error) => self.fail(error),
        }
    }

    /// Cancels the playback for `id`, discarding queued audio and the stream.
    ///
    /// Never emits `Drained`. A non-current id is ignored, including 0 while
    /// idle.
    pub fn stop(&mut self, id: u64) {
        if id != self.id {
            return;
        }
        self.id = 0;
        // Dropping the sink stops and closes the stream; it may join the
        // backend thread, so it happens outside every queue lock.
        self.sink = None;
        self.progress = None;
        self.queue = None;
        self.finished = false;
        self.started = false;
    }

    /// Applies what the device reported since the last call and emits the
    /// resulting events. Call it periodically, like the C++ 10 ms pump timer.
    pub fn pump(&mut self) {
        let Some(progress) = &self.progress else {
            return;
        };
        if progress.failed() {
            self.fail(Error::DEVICE);
            return;
        }
        let id = self.id;
        let started = progress.started();
        let drained = self.finished
            && self
                .queue
                .as_ref()
                .is_some_and(|queue| queue::lock(queue).is_empty())
            && progress.is_drained();
        if !self.started && started {
            // Qt's `ActiveState`: the stream is open and consuming.
            self.started = true;
            self.events.push(Event::PlaybackStarted(id));
            if self.id != id {
                return;
            }
        }
        if drained {
            self.stop(id);
            self.events.push(Event::Drained(id));
        }
    }

    /// Aborts the current playback and reports `error`, as the C++ `Fail`.
    fn fail(&mut self, error: Error) {
        let id = self.id;
        self.stop(id);
        if id != 0 {
            self.events.push(Event::Failed(id, error));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Records nothing; every player test that needs a device uses the
    /// integration-test fake in `tests/playback.rs`.
    struct NoDevice;

    impl SinkFactory for NoDevice {
        fn open(&mut self, _: SourceFormat, _: Shared) -> Result<Box<dyn Sink>, Error> {
            Err(Error::NO_DEVICE)
        }
    }

    const MONO: SourceFormat = SourceFormat {
        sample_rate: 24_000,
        channels: 1,
    };

    #[test]
    fn missing_device_fails_and_leaves_the_player_idle() {
        let mut player = Player::with_factory(NoDevice);
        player.start(7, MONO);
        assert_eq!(player.id(), 0);
        assert_eq!(
            player.take_events(),
            vec![Event::Failed(7, Error::NO_DEVICE)]
        );
        // A stale append/finish after the failure changes nothing.
        player.append(7, &[0, 0]);
        player.finish_input(7);
        player.pump();
        assert!(player.take_events().is_empty());
    }

    #[test]
    fn invalid_format_fails_before_opening_a_device() {
        struct Counting(Arc<()>);
        impl SinkFactory for Counting {
            fn open(&mut self, _: SourceFormat, _: Shared) -> Result<Box<dyn Sink>, Error> {
                let _ = &self.0;
                unreachable!("an invalid format must not reach the device")
            }
        }
        let mut player = Player::with_factory(Counting(Arc::new(())));
        player.start(
            1,
            SourceFormat {
                sample_rate: 0,
                channels: 1,
            },
        );
        assert_eq!(
            player.take_events(),
            vec![Event::Failed(1, Error::NO_DEVICE)]
        );
        player.start(
            2,
            SourceFormat {
                sample_rate: 24_000,
                channels: 0,
            },
        );
        assert_eq!(
            player.take_events(),
            vec![Event::Failed(2, Error::NO_DEVICE)]
        );
    }

    #[test]
    fn start_with_id_zero_opens_nothing() {
        struct Panics;
        impl SinkFactory for Panics {
            fn open(&mut self, _: SourceFormat, _: Shared) -> Result<Box<dyn Sink>, Error> {
                panic!("id 0 must not open a device")
            }
        }
        let mut player = Player::with_factory(Panics);
        player.start(0, MONO);
        assert_eq!(player.id(), 0);
        assert!(player.take_events().is_empty());
        assert_eq!(player.pending_bytes(), 0);
    }

    #[test]
    fn stop_with_id_zero_while_idle_is_harmless() {
        let mut player = Player::with_factory(NoDevice);
        player.stop(0);
        player.start(0, MONO);
        assert!(player.take_events().is_empty());
    }
}
