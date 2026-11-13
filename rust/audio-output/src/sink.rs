//! Device seam: the player talks to a [`Sink`] and tests inject their own.
//!
//! The real implementation ([`CpalFactory`]) opens the default output device
//! through cpal's default host (ALSA on Linux, which reaches PipeWire through
//! the `pipewire-alsa` plugin). A sink pulls from the shared [`Queue`] inside
//! its audio callback and reports progress through [`Progress`]; that type is
//! the seam that lets the queue and drain logic be tested without hardware.
//!
//! The callback never performs device I/O, allocation or logging: it locks the
//! queue for one callback buffer at a time, converts in place and returns.

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::time::Duration;

use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};

use crate::convert::Converter;
use crate::queue::{self, Shared};
use crate::{DeviceFormat, Error, SampleFormat, SourceFormat};

/// How long cpal may take to bring a stream up before it is reported as a
/// device failure. The C++ player inherits Qt's synchronous start.
const OPEN_TIMEOUT: Duration = Duration::from_secs(2);

/// Progress counters for one stream.
///
/// A callback marks everything delivered earlier as played and then delivers
/// what the queue had. `played == delivered` therefore means the device has
/// begun consuming the last frame handed to it, which is the Qt `IdleState`
/// condition the C++ `Pump` waits for: it only reported `Drained` once
/// `_sink->bytesFree()` had room for everything left in `_pending`, i.e. the
/// sink buffer had been handed to the device.
///
/// The callback marks the played watermark *before* filling, so it never
/// claims frames that were not yet written; the callback that delivers the
/// final frames cannot also report them consumed, and the next one can. That
/// costs at most one callback period before `Drained`, which is bounded by the
/// device buffer and is exactly the moment Qt reached `IdleState`.
///
/// `callbacks` counts how often the stream pulled data, which is the closest
/// cpal equivalent of Qt's `ActiveState` for [`Event::PlaybackStarted`].
#[derive(Debug, Default)]
pub struct Progress {
    delivered: AtomicU64,
    played: AtomicU64,
    callbacks: AtomicU64,
    failed: AtomicBool,
    /// Callback buffers whose audio was taken from the queue but whose frame
    /// count has not been published to `delivered` yet. The player must not
    /// report `Drained` while this is nonzero: the queue is already empty then,
    /// but the device has not been told about those frames.
    in_flight: AtomicU64,
}

impl Progress {
    /// No frames delivered and no failure.
    pub fn new() -> Self {
        Self::default()
    }

    /// Marks the start of one callback: earlier deliveries have been consumed.
    ///
    /// A sink that pulls audio manually from a test must call this before
    /// filling each buffer, exactly like a device callback.
    pub fn begin_callback(&self) {
        self.played
            .store(self.delivered.load(Ordering::Relaxed), Ordering::Relaxed);
        self.callbacks.fetch_add(1, Ordering::Relaxed);
    }

    /// Records frames just written into the callback buffer.
    pub fn delivered(&self, frames: u64) {
        self.delivered.fetch_add(frames, Ordering::Relaxed);
    }

    /// Marks the point between taking audio out of the queue and publishing how
    /// many frames it produced.
    ///
    /// A device callback must call [`Progress::begin_fill`] before it locks the
    /// queue and [`Progress::end_fill`] right after it publishes `delivered`.
    /// That makes "the queue is empty" and "every frame is counted" one atomic
    /// decision for [`crate::Player::pump`], instead of two observations a
    /// callback can be interrupted between — which used to report `Drained`
    /// while the last buffer was still in flight.
    pub fn begin_fill(&self) {
        self.in_flight.fetch_add(1, Ordering::AcqRel);
    }

    /// Closes the window opened by [`Progress::begin_fill`].
    pub fn end_fill(&self) {
        self.in_flight.fetch_sub(1, Ordering::AcqRel);
    }

    /// Whether a callback is between `begin_fill` and `end_fill`.
    pub fn filling(&self) -> bool {
        self.in_flight.load(Ordering::Acquire) != 0
    }

    /// Reports a stream error; the player surfaces it as [`Error::DEVICE`].
    pub fn fail(&self) {
        self.failed.store(true, Ordering::Relaxed);
    }

    /// Whether the stream reported an error.
    pub fn failed(&self) -> bool {
        self.failed.load(Ordering::Relaxed)
    }

    /// Whether the device has pulled audio at least once.
    pub fn started(&self) -> bool {
        self.callbacks.load(Ordering::Relaxed) > 0
    }

    /// Frames handed to the device since the playback started.
    pub fn delivered_frames(&self) -> u64 {
        self.delivered.load(Ordering::Relaxed)
    }

    /// Frames the device has consumed.
    pub fn played_frames(&self) -> u64 {
        self.played.load(Ordering::Relaxed)
    }

    /// Whether every delivered frame has been consumed by the device and no
    /// callback is mid-fill.
    pub fn is_drained(&self) -> bool {
        !self.filling() && self.played_frames() >= self.delivered_frames()
    }
}

/// A running output stream.
///
/// Dropping it stops and closes the stream without reporting anything, which is
/// what [`crate::Player::stop`] relies on.
pub trait Sink: Send {
    /// Format the stream actually runs at.
    fn format(&self) -> DeviceFormat;

    /// Shared progress counters updated by the stream callback.
    fn progress(&self) -> Arc<Progress>;
}

/// Opens output streams; the seam fake tests use instead of hardware.
pub trait SinkFactory {
    /// Opens the default output device for `source`, pulling from `queue`.
    ///
    /// Returns [`Error::NO_DEVICE`] when no device accepts the source sample
    /// rate, and [`Error::START`] when the stream cannot be started.
    fn open(&mut self, source: SourceFormat, queue: Shared) -> Result<Box<dyn Sink>, Error>;
}

impl std::fmt::Debug for dyn Sink {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Sink")
            .field("format", &self.format())
            .finish()
    }
}

/// cpal-backed factory over the default output device.
#[derive(Debug, Default)]
pub struct CpalFactory;

/// Maps a cpal sample format onto the encodings this crate can produce.
///
/// I24/U24 are excluded: cpal stores them in four bytes with an unused high
/// byte, which this crate's encoders do not target.
fn sample_format(format: cpal::SampleFormat) -> Option<SampleFormat> {
    match format {
        cpal::SampleFormat::I8 => Some(SampleFormat::I8),
        cpal::SampleFormat::U8 => Some(SampleFormat::U8),
        cpal::SampleFormat::I16 => Some(SampleFormat::I16),
        cpal::SampleFormat::U16 => Some(SampleFormat::U16),
        cpal::SampleFormat::I32 => Some(SampleFormat::I32),
        cpal::SampleFormat::U32 => Some(SampleFormat::U32),
        cpal::SampleFormat::F32 => Some(SampleFormat::F32),
        cpal::SampleFormat::F64 => Some(SampleFormat::F64),
        _ => None,
    }
}

fn cpal_format(format: SampleFormat) -> cpal::SampleFormat {
    match format {
        SampleFormat::I8 => cpal::SampleFormat::I8,
        SampleFormat::U8 => cpal::SampleFormat::U8,
        SampleFormat::I16 => cpal::SampleFormat::I16,
        SampleFormat::U16 => cpal::SampleFormat::U16,
        SampleFormat::I32 => cpal::SampleFormat::I32,
        SampleFormat::U32 => cpal::SampleFormat::U32,
        SampleFormat::F32 => cpal::SampleFormat::F32,
        SampleFormat::F64 => cpal::SampleFormat::F64,
    }
}

/// Default output device.
///
/// `cpal::default_host()` panics when no host can be constructed, so hosts are
/// selected through the fallible `host_from_id`. The default host is tried
/// first; any other available host only runs when it has no default output.
fn default_device() -> Option<(cpal::Device, cpal::SupportedStreamConfig)> {
    let default = cpal::default_host().id();
    let hosts = cpal::available_hosts();
    let ordered = std::iter::once(default).chain(hosts.into_iter().filter(|host| *host != default));
    for host_id in ordered {
        let Ok(host) = cpal::host_from_id(host_id) else {
            continue;
        };
        let Some(device) = host.default_output_device() else {
            continue;
        };
        if let Ok(config) = device.default_output_config() {
            return Some((device, config));
        }
    }
    None
}

impl SinkFactory for CpalFactory {
    fn open(&mut self, source: SourceFormat, queue: Shared) -> Result<Box<dyn Sink>, Error> {
        if !source.is_valid() {
            return Err(Error::NO_DEVICE);
        }
        let Some((device, default)) = default_device() else {
            return Err(Error::NO_DEVICE);
        };
        let Some(native) = sample_format(default.sample_format()) else {
            return Err(Error::NO_DEVICE);
        };
        // The source sample rate is never converted by this crate. The stream
        // asks for the source rate; PipeWire's graph (or ALSA's plug layer)
        // either accepts it and resamples downstream, or refuses to open, in
        // which case the caller reports `AudioDevice` just like Qt's
        // `isFormatSupported` gate did.
        let mut candidates = Vec::with_capacity(4);
        for channels in [default.channels(), source.channels] {
            for encoding in [native, SampleFormat::I16] {
                let candidate = (channels, encoding);
                if !candidates.contains(&candidate) {
                    candidates.push(candidate);
                }
            }
        }
        let mut started = false;
        for (channels, encoding) in candidates {
            if channels == 0 || channels > 32 {
                continue;
            }
            let format = DeviceFormat {
                sample_rate: source.sample_rate,
                channels,
                sample_format: encoding,
            };
            let Some(converter) = Converter::new(source, format) else {
                continue;
            };
            let config = cpal::StreamConfig {
                channels,
                sample_rate: source.sample_rate,
                buffer_size: cpal::BufferSize::Default,
            };
            let progress = Arc::new(Progress::new());
            let callback_progress = Arc::clone(&progress);
            let devices = Arc::clone(&progress);
            let callback_queue = Arc::clone(&queue);
            let callback = move |data: &mut cpal::Data, _: &cpal::OutputCallbackInfo| {
                callback_progress.begin_callback();
                let bytes = data.bytes_mut();
                // Taking audio out of the queue and publishing its frame count
                // must be one step for the drain check; see `Progress::begin_fill`.
                callback_progress.begin_fill();
                let filled = {
                    let mut queue = queue::lock(&callback_queue);
                    queue.fill(&converter, bytes)
                };
                callback_progress.delivered(filled as u64);
                callback_progress.end_fill();
            };
            let errors = move |_: cpal::Error| devices.fail();
            let stream = match device.build_output_stream_raw(
                config,
                cpal_format(encoding),
                callback,
                errors,
                Some(OPEN_TIMEOUT),
            ) {
                Ok(stream) => stream,
                Err(_) => continue,
            };
            started = true;
            if stream.play().is_err() {
                break;
            }
            return Ok(Box::new(CpalSink {
                _stream: stream,
                format,
                progress,
            }));
        }
        Err(if started {
            Error::START
        } else {
            Error::NO_DEVICE
        })
    }
}

struct CpalSink {
    /// Kept alive for playback; dropping it closes the stream and joins the
    /// backend thread, so it is never read directly.
    _stream: cpal::Stream,
    format: DeviceFormat,
    progress: Arc<Progress>,
}

impl std::fmt::Debug for CpalSink {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("CpalSink")
            .field("format", &self.format)
            .finish()
    }
}

impl Sink for CpalSink {
    fn format(&self) -> DeviceFormat {
        self.format
    }

    fn progress(&self) -> Arc<Progress> {
        Arc::clone(&self.progress)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn progress_starts_and_drains_exactly_once_per_callback() {
        let progress = Progress::new();
        assert!(progress.is_drained());
        assert!(!progress.started());

        progress.begin_callback();
        progress.delivered(256);
        assert!(progress.started());
        assert!(!progress.is_drained());

        progress.begin_callback();
        assert!(progress.is_drained());
        assert_eq!(progress.delivered_frames(), 256);
        assert_eq!(progress.played_frames(), 256);

        progress.begin_callback();
        progress.delivered(128);
        progress.begin_callback();
        progress.delivered(64);
        assert!(!progress.is_drained());
        progress.begin_callback();
        assert!(progress.is_drained());
        assert_eq!(progress.played_frames(), 448);

        progress.fail();
        assert!(progress.failed());
    }

    #[test]
    fn encodings_round_trip_through_the_cpal_mapping() {
        for format in [
            SampleFormat::I8,
            SampleFormat::U8,
            SampleFormat::I16,
            SampleFormat::U16,
            SampleFormat::I32,
            SampleFormat::U32,
            SampleFormat::F32,
            SampleFormat::F64,
        ] {
            assert_eq!(sample_format(cpal_format(format)), Some(format));
        }
        assert_eq!(sample_format(cpal::SampleFormat::I24), None);
        assert_eq!(sample_format(cpal::SampleFormat::U24), None);
    }
}
