//! Microphone capture on the default or a named input device, ported from the
//! Qt Multimedia capture in
//! `plugins/voice_input/private/audio/AudioCapture.cpp` and its `PcmNormalizer`.
//!
//! The crate owns only the capture engine: device enumeration, format
//! negotiation, conversion of whatever the device delivers into 16 kHz mono
//! PCM16 little-endian, the recording bounds, and the same terminal failure the
//! C++ `AudioCapture::Failed` signal reported. It contains no Qt, no plugin
//! lifecycle, no ASR and no clipboard code.
//!
//! # Contract (mirrors `AudioCapture`)
//!
//! * [`Capture::list_devices`] returns `id` / `name` / `is_default` entries, the
//!   keys `plugins/voice_input/gui/VoiceInputTab.cpp` reads.
//! * [`Capture::start`] requires an idle capture and a 1–300 s limit. An empty
//!   device selects the current default input; a named device that is not in
//!   the list fails with [`Error::UNAVAILABLE`] and never falls back to another
//!   microphone.
//! * Captured audio is converted to 16 kHz mono PCM16: channels are averaged
//!   and the rate is resampled. A device already running at 16 kHz mono PCM16
//!   is copied byte for byte.
//! * [`Capture::pump`] drains what the device buffered. Reaching the recording
//!   budget truncates further input and sets [`Capture::budget_reached`]; the
//!   owner stops normally, exactly like the C++ controller's timer.
//! * [`Capture::stop`] drains the tail and returns the PCM; [`Capture::cancel`]
//!   discards it. Both reject a session that is not current.
//! * A device or conversion failure stops the capture and queues
//!   [`Event::Failed`], which the owner collects with [`Capture::take_events`].
//!
//! Unlike the C++ object, failures are synchronous: they surface from
//! [`Capture::pump`] or [`Capture::stop`] instead of a queued Qt signal, so the
//! generation counter that guarded stale queued failures is unnecessary.
//!
//! ```no_run
//! use deskwaifu_audio_capture::Capture;
//!
//! let mut capture = Capture::new();
//! let devices = capture.list_devices();
//! let device = devices.iter().find(|d| d.is_default).map(|d| d.id.clone());
//! capture.start(1, device.as_deref().unwrap_or(""), 60).unwrap();
//! capture.pump();
//! let pcm = capture.stop(1).unwrap();
//! assert!(pcm.len().is_multiple_of(2));
//! ```

mod base64;
mod capture;
mod device;
mod normalize;

pub use capture::{Capture, Clock, SystemClock, MAX_SECONDS};
pub use device::CpalFactory;

/// Largest 16 kHz mono PCM16 recording, the C++ `MaxPcmBytes`.
pub const MAX_PCM_BYTES: usize = 300 * 16_000 * 2;

/// Sample encoding requested from, or delivered by, the input device.
///
/// Mirrors the subset of `cpal::SampleFormat` this crate can decode. I24/U24,
/// I64/U64 and the DSD containers are rejected as an unsupported input format,
/// like the C++ normalizer rejected anything outside
/// `UInt8`/`Int16`/`Int32`/`Float`.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SampleFormat {
    I8,
    U8,
    I16,
    U16,
    I32,
    U32,
    F32,
    F64,
}

impl SampleFormat {
    /// Bytes in one sample of this encoding.
    pub const fn sample_bytes(self) -> usize {
        match self {
            Self::I8 | Self::U8 => 1,
            Self::I16 | Self::U16 => 2,
            Self::I32 | Self::U32 | Self::F32 => 4,
            Self::F64 => 8,
        }
    }
}

/// Interleaved format of the device stream.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CaptureFormat {
    pub sample_rate: u32,
    pub channels: u16,
    pub sample_format: SampleFormat,
}

impl CaptureFormat {
    /// Format the C++ capture asked for first: 16 kHz mono PCM16.
    pub const REQUESTED: Self = Self {
        sample_rate: 16_000,
        channels: 1,
        sample_format: SampleFormat::I16,
    };

    /// Bytes in one interleaved device frame.
    pub const fn frame_bytes(self) -> usize {
        self.channels as usize * self.sample_format.sample_bytes()
    }

    /// Whether the C++ `PcmNormalizer::Reset` would accept this format.
    pub const fn is_valid(self) -> bool {
        self.sample_rate != 0
            && self.sample_rate <= 384_000
            && self.channels != 0
            && self.channels <= 32
    }
}

/// One entry of the device list.
///
/// `id` is the opaque base64url token the GUI stores in `device_id`; it is the
/// base64url encoding of the cpal device identifier, matching the C++ encoder
/// (`QByteArray::toBase64(Base64UrlEncoding)`).
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DeviceInfo {
    pub id: String,
    pub name: String,
    pub is_default: bool,
}

/// Failure classification.
///
/// `Limit` and `Device` carry the exact `audio_limit` / `audio_device` codes the
/// C++ object emitted; the remaining variants classify the `bool + QString`
/// start/stop failures, which had no code in C++.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ErrorCode {
    /// `Start` while a capture is running.
    Busy,
    /// `maxSeconds` outside 1–300.
    InvalidDuration,
    /// The selected input device is missing or cannot be opened.
    Unavailable,
    /// The device format cannot be decoded.
    UnsupportedFormat,
    /// The input stream could not be started.
    Start,
    /// `Stop` for an id that is not the current session.
    StaleSession,
    /// The C++ `audio_limit` failure code.
    Limit,
    /// The C++ `audio_device` failure code.
    Device,
    /// The PCM accumulator rejected the data structurally.
    Pcm,
}

/// Fixed failure description, delivered as code and message.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Error {
    pub code: ErrorCode,
    pub message: &'static str,
}

impl Error {
    /// An input is already being captured.
    pub const BUSY: Self = Self {
        code: ErrorCode::Busy,
        message: "Audio capture is busy",
    };
    /// The requested duration is outside the 1–300 s window.
    pub const INVALID_DURATION: Self = Self {
        code: ErrorCode::InvalidDuration,
        message: "Invalid recording duration",
    };
    /// The named or default input device is not available.
    pub const UNAVAILABLE: Self = Self {
        code: ErrorCode::Unavailable,
        message: "Audio input device is unavailable",
    };
    /// The device delivers a format the normalizer cannot decode.
    pub const UNSUPPORTED_FORMAT: Self = Self {
        code: ErrorCode::UnsupportedFormat,
        message: "Unsupported input audio format",
    };
    /// The input stream could not be started.
    pub const START: Self = Self {
        code: ErrorCode::Start,
        message: "Could not start audio input",
    };
    /// `stop` carried an id that is not the current session.
    pub const STALE: Self = Self {
        code: ErrorCode::StaleSession,
        message: "Stale audio capture session",
    };
    /// The recording exceeded its duration budget.
    pub const LIMIT: Self = Self {
        code: ErrorCode::Limit,
        message: "Recording duration limit exceeded",
    };
    /// The device reported a stream error.
    pub const DEVICE: Self = Self {
        code: ErrorCode::Device,
        message: "Audio input device failed",
    };
    /// The converted PCM would exceed [`MAX_PCM_BYTES`].
    pub const BUFFER_LIMIT: Self = Self {
        code: ErrorCode::Limit,
        message: "Audio buffer limit exceeded",
    };
    /// One `append` carried more than 4 MiB.
    pub const INPUT_TOO_LARGE: Self = Self {
        code: ErrorCode::Limit,
        message: "Audio input block too large",
    };
    /// The resampler could not be created.
    pub const RESAMPLE_INIT: Self = Self {
        code: ErrorCode::Limit,
        message: "Audio resampler initialization failed",
    };
    /// libsamplerate reported an error.
    pub const RESAMPLE_FAILED: Self = Self {
        code: ErrorCode::Limit,
        message: "Audio resampling failed",
    };
    /// libsamplerate made no progress.
    pub const RESAMPLE_STALLED: Self = Self {
        code: ErrorCode::Limit,
        message: "Audio resampling stalled",
    };
    /// The normalizer is not primed.
    pub const INACTIVE: Self = Self {
        code: ErrorCode::Pcm,
        message: "Audio normalizer is inactive",
    };
    /// The captured bytes end in a partial native frame.
    pub const INCOMPLETE_FRAME: Self = Self {
        code: ErrorCode::Pcm,
        message: "Incomplete audio sample frame",
    };
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.message)
    }
}

impl std::error::Error for Error {}

/// Outcome the owner collects, mirroring the C++ `Failed` signal.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Event {
    /// `Failed(sessionId, code, message)`: the capture was aborted and cleaned
    /// up.
    Failed(u64, Error),
}

/// An opened input stream.
pub struct Opened {
    /// Format the stream actually runs at.
    pub format: CaptureFormat,
    /// The running stream.
    pub source: Box<dyn Source>,
}

/// A running capture stream.
///
/// Dropping it stops and closes the stream without reporting anything, which is
/// what [`Capture::stop`] and [`Capture::cancel`] rely on.
pub trait Source: Send {
    /// Format the stream actually runs at.
    fn format(&self) -> CaptureFormat;

    /// Removes and returns the bytes captured since the last call.
    fn take(&mut self) -> Vec<u8>;

    /// Error the backend reported since the stream started.
    ///
    /// Once set, the capture stops instead of uploading a partial recording.
    fn error(&self) -> Option<Error>;
}

impl std::fmt::Debug for dyn Source {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Source")
            .field("format", &self.format())
            .finish()
    }
}

/// Opens input streams and enumerates devices; the seam fake tests use.
pub trait CaptureFactory {
    /// Input devices in the order the backend reports them.
    ///
    /// Mirrors `AudioCapture::ListDevices`, which never fails: an enumeration
    /// error yields no entries.
    fn devices(&mut self) -> Vec<DeviceInfo>;

    /// Opens `device`, or the current default input when `None`.
    ///
    /// `requested` is the preferred format (16 kHz mono PCM16); the
    /// implementation negotiates the actual stream format and reports it in
    /// [`Opened::format`]. A `Some` id always comes from [`Self::devices`] and
    /// must not fall back to another device: returning [`Error::UNAVAILABLE`]
    /// is the only correct answer when it cannot be opened.
    fn open(&mut self, device: Option<&str>, requested: CaptureFormat) -> Result<Opened, Error>;
}
