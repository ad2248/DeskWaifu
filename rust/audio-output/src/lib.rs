//! PCM playback on the default audio output, ported from the Qt Multimedia
//! player in `plugins/speech/private/processor/speech/audio/AudioPlayer.cpp`.
//!
//! The crate owns only the playback engine: it buffers interleaved PCM16,
//! feeds the negotiated device stream and reports the same terminal outcomes
//! the C++ player emits as Qt signals ([`Event::PlaybackStarted`],
//! [`Event::Drained`], [`Event::Failed`]). It contains no network, Qt or
//! plugin lifecycle code.
//!
//! # Contract (mirrors `AudioPlayer`)
//!
//! * [`Player::start`] discards any previous playback, then opens the default
//!   output device at the requested sample rate. The device keeps its own
//!   channel count and sample format; the source samples are converted to it.
//! * [`Player::append`] queues bytes for the active id only. An id that is not
//!   current, input after [`Player::finish_input`] and id 0 are ignored.
//!   Pending bytes above [`MAX_PENDING_BYTES`] fail with
//!   [`ErrorCode::BufferOverflow`].
//! * [`Player::finish_input`] marks end of input. Audio already queued keeps
//!   playing; [`Event::Drained`] is reported only after every queued frame has
//!   been handed to the device *and* the device has played it.
//! * [`Player::stop`] drops the queue and the stream without reporting
//!   `Drained`.
//! * Ids are single-use: any call carrying another id is ignored completely.
//!
//! Unlike the C++ player, feeding the device is driven by the audio callback
//! and outcomes are collected by calling [`Player::pump`], so no Qt event loop
//! or timer is involved. The owner is expected to call `pump` on the same
//! cadence as the C++ 10 ms timer.
//!
//! ```
//! use deskwaifu_audio_output::{Player, SourceFormat};
//!
//! let mut player = Player::new();
//! player.start(1, SourceFormat { sample_rate: 24_000, channels: 1 });
//! player.append(1, &[0; 480]);
//! player.finish_input(1);
//! // `player.pump()` reports Drained once the device has played the buffer.
//! ```
//!
//! # Deviations from the C++ player
//!
//! * Qt rejects a device that does not support the requested format, including
//!   a different channel count or sample format. cpal requires an explicit
//!   sample type and channel count, so those two are negotiated to the device
//!   and converted. The sample rate is requested as-is and never resampled
//!   here; a stream the backend refuses to open is reported as
//!   [`ErrorCode::AudioDevice`], like Qt's `isFormatSupported` rejection.
//! * Id 0 never opens a device. The C++ `Start(0, format)` opens a sink that no
//!   `Pump` ever feeds; nothing observable can come out of it either way.

mod convert;
mod player;
mod queue;
mod sink;

pub use convert::Converter;
pub use player::{Event, Player};
pub use queue::{Queue, Shared};
pub use sink::{CpalFactory, Progress, Sink, SinkFactory};
/// Pending bytes accepted for one playback before it is reported as overflow.
///
/// Mirrors the C++ `8 * 1024 * 1024` limit, measured in source bytes that have
/// not yet been handed to the device.
pub const MAX_PENDING_BYTES: usize = 8 * 1024 * 1024;

/// Format of the PCM handed to [`Player::append`].
///
/// Samples are signed 16-bit interleaved little-endian values, the only format
/// the speech source produces (`QAudioFormat::Int16` in the C++ player).
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SourceFormat {
    pub sample_rate: u32,
    pub channels: u16,
}

impl SourceFormat {
    /// Bytes in one interleaved source frame.
    pub const fn frame_bytes(self) -> usize {
        self.channels as usize * 2
    }

    /// Whether the format describes a usable stream.
    pub const fn is_valid(self) -> bool {
        self.sample_rate != 0 && self.channels != 0 && self.channels <= 32
    }
}

/// Sample encoding cpal was asked to open on the device.
///
/// Mirrors the subset of `cpal::SampleFormat` this crate can encode.
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

/// Format the device stream was opened with.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DeviceFormat {
    pub sample_rate: u32,
    pub channels: u16,
    pub sample_format: SampleFormat,
}

impl DeviceFormat {
    /// Bytes in one interleaved device frame.
    pub const fn frame_bytes(self) -> usize {
        self.channels as usize * self.sample_format.sample_bytes()
    }
}

/// Failure classification, mapped from `SpeechErrorCode`.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ErrorCode {
    /// `SpeechErrorCode::AudioDevice`: no device, unusable format or a stream error.
    AudioDevice,
    /// `SpeechErrorCode::BufferOverflow`: the pending queue would exceed its limit.
    BufferOverflow,
    /// `SpeechErrorCode::Protocol`: the queued PCM is not a whole number of frames.
    Protocol,
}

/// Fixed failure description, delivered as `SpeechError { code, message }`.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Error {
    pub code: ErrorCode,
    pub message: &'static str,
}

impl Error {
    /// `Start` found no output device able to take the speech format.
    pub const NO_DEVICE: Self = Self {
        code: ErrorCode::AudioDevice,
        message: "没有支持语音格式的音频输出设备",
    };
    /// The device stream could not be started.
    pub const START: Self = Self {
        code: ErrorCode::AudioDevice,
        message: "无法启动音频输出",
    };
    /// The device reported an error or failed a write after starting.
    pub const DEVICE: Self = Self {
        code: ErrorCode::AudioDevice,
        message: "音频输出设备发生错误",
    };
    /// Pending audio exceeded [`MAX_PENDING_BYTES`].
    pub const OVERFLOW: Self = Self {
        code: ErrorCode::BufferOverflow,
        message: "语音播放缓冲区已满",
    };
    /// `finish_input` saw a trailing partial frame.
    pub const INCOMPLETE_FRAME: Self = Self {
        code: ErrorCode::Protocol,
        message: "PCM 音频帧不完整",
    };
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.message)
    }
}

impl std::error::Error for Error {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn format_sizes_match_interleaved_layout() {
        let mono = SourceFormat {
            sample_rate: 24_000,
            channels: 1,
        };
        assert_eq!(mono.frame_bytes(), 2);
        assert_eq!(
            SourceFormat {
                channels: 2,
                ..mono
            }
            .frame_bytes(),
            4
        );
        assert!(!SourceFormat {
            sample_rate: 0,
            ..mono
        }
        .is_valid());
        assert!(!SourceFormat {
            channels: 0,
            ..mono
        }
        .is_valid());
        assert!(!SourceFormat {
            channels: 33,
            ..mono
        }
        .is_valid());
        let format = DeviceFormat {
            sample_rate: 48_000,
            channels: 2,
            sample_format: SampleFormat::F64,
        };
        assert_eq!(format.frame_bytes(), 16);
        assert_eq!(SampleFormat::U8.sample_bytes(), 1);
        assert_eq!(SampleFormat::F32.sample_bytes(), 4);
    }

    #[test]
    fn errors_carry_the_cpp_message_and_code() {
        assert_eq!(Error::NO_DEVICE.code, ErrorCode::AudioDevice);
        assert_eq!(Error::START.code, ErrorCode::AudioDevice);
        assert_eq!(Error::DEVICE.code, ErrorCode::AudioDevice);
        assert_eq!(Error::OVERFLOW.code, ErrorCode::BufferOverflow);
        assert_eq!(Error::INCOMPLETE_FRAME.code, ErrorCode::Protocol);
        assert_eq!(
            Error::NO_DEVICE.to_string(),
            "没有支持语音格式的音频输出设备"
        );
        assert_eq!(Error::START.to_string(), "无法启动音频输出");
        assert_eq!(Error::DEVICE.to_string(), "音频输出设备发生错误");
        assert_eq!(Error::OVERFLOW.to_string(), "语音播放缓冲区已满");
        assert_eq!(Error::INCOMPLETE_FRAME.to_string(), "PCM 音频帧不完整");
    }
}
