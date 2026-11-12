//! Bounded pending-audio queue shared between the caller and the device.
//!
//! The queue stores the *source* bytes exactly as [`crate::Player::append`]
//! received them, so the C++ limit of 8 MiB on `_pending` maps one-to-one and
//! conversion happens on the way out. Both users hold the mutex only for a
//! single memcpy or one callback buffer, never across a device call.
//!
//! Bytes are kept in a flat [`Vec`] with a read cursor so that draining a
//! callback buffer is one `copy_from_slice` from contiguous memory, and the
//! unused prefix is reclaimed once it dominates the queue.

use std::sync::{Arc, Mutex, MutexGuard};

use crate::convert::{encode_silence, Converter};
use crate::{Error, MAX_PENDING_BYTES};

/// Queue shared by the player and the device callback.
pub type Shared = Arc<Mutex<Queue>>;

/// Creates the shared queue used for one playback.
pub(crate) fn shared() -> Shared {
    Arc::new(Mutex::new(Queue::new()))
}

/// Locks without panicking when another user panicked while holding the lock.
///
/// Conversion is infallible and the queue is only mutated after a successful
/// copy, so a poisoned lock still describes a consistent queue. Ignoring the
/// poison keeps a device failure from becoming a caller-side panic.
pub(crate) fn lock(queue: &Shared) -> MutexGuard<'_, Queue> {
    queue.lock().unwrap_or_else(|error| error.into_inner())
}

/// Pending source PCM plus end-of-input state.
#[derive(Debug)]
pub struct Queue {
    data: Vec<u8>,
    start: usize,
    finished: bool,
}

impl Default for Queue {
    fn default() -> Self {
        Self::new()
    }
}

impl Queue {
    /// Empty queue that accepts up to [`MAX_PENDING_BYTES`].
    pub fn new() -> Self {
        Self {
            data: Vec::new(),
            start: 0,
            finished: false,
        }
    }

    /// Pending source bytes.
    pub fn len(&self) -> usize {
        self.data.len() - self.start
    }

    /// Whether nothing is pending.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Whether [`Queue::finish`] already happened.
    pub fn finished(&self) -> bool {
        self.finished
    }

    /// Maximum number of pending source bytes.
    pub const fn limit(&self) -> usize {
        MAX_PENDING_BYTES
    }

    /// Appends source bytes, or reports [`Error::OVERFLOW`] without changing
    /// the queue when the result would pass the limit.
    ///
    /// The C++ guard is `_pending.size() + pcm.size() > 8 MiB`, so exactly
    /// 8 MiB is accepted and one more byte is not. A zero-length append is
    /// therefore always accepted and changes nothing.
    pub fn append(&mut self, bytes: &[u8]) -> Result<(), Error> {
        let total = self.len().saturating_add(bytes.len());
        if total > MAX_PENDING_BYTES {
            return Err(Error::OVERFLOW);
        }
        if self.start > 0 && self.start >= self.data.len() / 2 {
            self.data.drain(..self.start);
            self.start = 0;
        }
        self.data.extend_from_slice(bytes);
        Ok(())
    }

    /// Marks end of input after checking that the pending bytes form whole
    /// frames of `frame_bytes`.
    pub fn finish(&mut self, frame_bytes: usize) -> Result<(), Error> {
        if frame_bytes == 0 || !self.len().is_multiple_of(frame_bytes) {
            return Err(Error::INCOMPLETE_FRAME);
        }
        self.finished = true;
        Ok(())
    }

    /// Converts as many whole source frames as fit into `out` and silences the
    /// rest, returning the number of device frames that received audio.
    ///
    /// A trailing partial source frame stays queued until later bytes complete
    /// it; the C++ player could not write a partial frame either and only
    /// rejected it in `FinishInput`.
    pub fn fill(&mut self, converter: &Converter, out: &mut [u8]) -> usize {
        let source_frame = converter.source().frame_bytes();
        let device_frame = converter.device().frame_bytes();
        let usable = self.len() / source_frame;
        let frames = usable.min(out.len() / device_frame);
        let bytes = frames * source_frame;
        if frames > 0 {
            let source = &self.data[self.start..self.start + bytes];
            converter.write(source, &mut out[..frames * device_frame]);
            self.start += bytes;
            if self.start == self.data.len() {
                self.data.clear();
                self.start = 0;
            }
        }
        encode_silence(converter.device(), &mut out[frames * device_frame..]);
        frames
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{DeviceFormat, SampleFormat, SourceFormat};

    const SOURCE: SourceFormat = SourceFormat {
        sample_rate: 24_000,
        channels: 1,
    };
    const DEVICE: DeviceFormat = DeviceFormat {
        sample_rate: 24_000,
        channels: 2,
        sample_format: SampleFormat::F32,
    };

    fn converter() -> Converter {
        Converter::new(SOURCE, DEVICE).unwrap()
    }

    #[test]
    fn accepts_exactly_the_limit_and_rejects_one_more_byte() {
        let mut queue = Queue::new();
        assert_eq!(queue.limit(), MAX_PENDING_BYTES);
        queue.append(&vec![0; MAX_PENDING_BYTES]).unwrap();
        assert_eq!(queue.len(), MAX_PENDING_BYTES);
        assert_eq!(queue.append(&[0]), Err(Error::OVERFLOW));
        assert_eq!(queue.len(), MAX_PENDING_BYTES);
        assert_eq!(queue.append(&[]), Ok(()));

        let mut queue = Queue::new();
        queue.append(&vec![1; MAX_PENDING_BYTES - 1]).unwrap();
        queue.append(&[2]).unwrap();
        assert_eq!(queue.append(&[3]), Err(Error::OVERFLOW));
    }

    #[test]
    fn overflow_keeps_the_previous_contents() {
        let mut queue = Queue::new();
        queue.append(&[7, 8]).unwrap();
        assert_eq!(
            queue.append(&vec![0; MAX_PENDING_BYTES]),
            Err(Error::OVERFLOW)
        );
        assert_eq!(queue.len(), 2);
        assert!(!queue.finished());
    }

    #[test]
    fn fill_converts_whole_frames_and_silences_the_remainder() {
        let mut queue = Queue::new();
        queue.append(&[0x00, 0x40, 0x00, 0xc0]).unwrap(); // 0.5, -0.5
        let mut out = vec![0xff; 6 * 4];
        assert_eq!(queue.fill(&converter(), &mut out), 2);
        assert!(queue.is_empty());
        let values: Vec<f32> = out
            .as_chunks::<4>()
            .0
            .iter()
            .map(|c| f32::from_le_bytes(*c))
            .collect();
        assert_eq!(values, vec![0.5, 0.5, -0.5, -0.5, 0.0, 0.0]);
    }

    #[test]
    fn fill_leaves_a_partial_frame_for_the_next_append() {
        let mut queue = Queue::new();
        queue.append(&[0x10, 0x00, 0x20]).unwrap();
        let mut out = vec![0u8; 8];
        assert_eq!(queue.fill(&converter(), &mut out), 1);
        assert_eq!(queue.len(), 1);
        queue.append(&[0x00]).unwrap();
        assert_eq!(queue.fill(&converter(), &mut out), 1);
        assert!(queue.is_empty());
    }

    #[test]
    fn fill_with_an_empty_queue_writes_only_silence() {
        let mut queue = Queue::new();
        let mut out = vec![0xff; 8];
        assert_eq!(queue.fill(&converter(), &mut out), 0);
        assert_eq!(out, vec![0u8; 8]);
    }

    #[test]
    fn finish_requires_whole_source_frames() {
        let mut queue = Queue::new();
        queue.append(&[0, 0]).unwrap();
        queue.finish(2).unwrap();
        assert!(queue.finished());

        let mut queue = Queue::new();
        queue.append(&[0]).unwrap();
        assert_eq!(queue.finish(2), Err(Error::INCOMPLETE_FRAME));
        assert!(!queue.finished());
    }
}
