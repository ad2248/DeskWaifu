//! Source PCM16 → device sample format conversion.
//!
//! The C++ player only ever hands the device bytes in the device's own format:
//! Qt refuses a `QAudioFormat` the device does not support, so it never
//! converts channels or sample formats. cpal has no such implicit conversion,
//! so this module performs it explicitly.
//!
//! Sample values are normalized to `f32` in `[-1, 1]` and encoded with the
//! scaling used by the rest of the repo (`deskwaifu-audio-domain`): an `i16`
//! sample `s` maps to `s / 32768`, and re-encoding rounds half away from zero
//! and clamps. Non-finite results encode as silence.
//!
//! Channel mapping is fixed:
//!
//! * one source channel to N device channels: the mono sample is copied to
//!   every channel (no attenuation, matching how a mono speech track is
//!   normally presented on a stereo sink);
//! * several source channels to one device channel: the channels are averaged
//!   in `f64`, the down-mix rule of `deskwaifu_audio_domain::normalize_pcm`;
//! * several source channels to several device channels: device channel `c`
//!   reads source channel `min(c, source_channels - 1)`, so a narrower source
//!   is duplicated into the extra channels and a wider source keeps its
//!   lowest device-channel-count channels.
//!
//! Sample rate is never converted: [`Converter::new`] rejects a mismatch so the
//! caller can report it as an unsupported device format, as Qt's
//! `isFormatSupported` did.

use crate::{DeviceFormat, SampleFormat, SourceFormat};

/// Converts whole interleaved PCM16 frames into the device encoding.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Converter {
    source: SourceFormat,
    device: DeviceFormat,
}

impl Converter {
    /// Creates a converter, or `None` when the rates differ or a channel count
    /// is unusable.
    pub fn new(source: SourceFormat, device: DeviceFormat) -> Option<Self> {
        if !source.is_valid()
            || device.sample_rate != source.sample_rate
            || device.channels == 0
            || device.channels > 32
        {
            return None;
        }
        Some(Self { source, device })
    }

    /// Format of the appended data.
    pub const fn source(&self) -> SourceFormat {
        self.source
    }

    /// Encoding written to the device.
    pub const fn device(&self) -> DeviceFormat {
        self.device
    }

    /// Whole source frames contained in `input`.
    pub fn frames(&self, input: &[u8]) -> usize {
        input.len() / self.source.frame_bytes()
    }

    /// Converts whole frames from `input` into `output`, which must be exactly
    /// `frames * self.device().frame_bytes()` long. Returns the frames written.
    ///
    /// A trailing partial source frame is ignored.
    pub fn write(&self, input: &[u8], output: &mut [u8]) -> usize {
        let source_frame = self.source.frame_bytes();
        let device_frame = self.device.frame_bytes();
        let sample_bytes = self.device.sample_format.sample_bytes();
        let frames = self.frames(input).min(output.len() / device_frame);
        for (frame, out) in input
            .chunks_exact(source_frame)
            .zip(output.chunks_exact_mut(device_frame))
            .take(frames)
        {
            for (channel, sample) in out.chunks_exact_mut(sample_bytes).enumerate() {
                let value = channel_value(
                    frame,
                    self.source.channels,
                    self.device.channels,
                    channel as u16,
                );
                encode(value, self.device.sample_format, sample);
            }
        }
        frames
    }

    /// Converts whole frames into a fresh buffer.
    pub fn convert(&self, input: &[u8]) -> Vec<u8> {
        let frames = self.frames(input);
        let mut output = vec![0; frames * self.device.frame_bytes()];
        let written = self.write(input, &mut output);
        output.truncate(written * self.device.frame_bytes());
        output
    }
}

/// Reads the source channels of one frame and selects `channel` of the device.
///
/// A multi-channel source feeding a mono device is averaged in `f64`, matching
/// `deskwaifu_audio_domain::normalize_pcm`; every other case is a direct
/// mapping or the duplication described in the module documentation.
fn channel_value(frame: &[u8], source_channels: u16, device_channels: u16, channel: u16) -> f32 {
    let sources = usize::from(source_channels);
    if sources == 1 {
        return decode(frame);
    }
    if device_channels == 1 {
        let sum: f64 = (0..sources)
            .map(|i| f64::from(decode(&frame[i * 2..i * 2 + 2])))
            .sum();
        let average = (sum / sources as f64) as f32;
        return if average.is_finite() {
            average.clamp(-1.0, 1.0)
        } else {
            0.0
        };
    }
    let channel = usize::from(channel).min(sources - 1);
    decode(&frame[channel * 2..channel * 2 + 2])
}

fn decode(sample: &[u8]) -> f32 {
    f32::from(i16::from_le_bytes([sample[0], sample[1]])) / 32_768.0
}

fn encode(sample: f32, format: SampleFormat, output: &mut [u8]) {
    let value = if sample.is_finite() {
        sample.clamp(-1.0, 1.0)
    } else {
        0.0
    };
    match format {
        SampleFormat::I8 | SampleFormat::U8 => {
            output[0] = if format == SampleFormat::I8 {
                scaled_signed(value, 128.0, i8::MIN as f64, i8::MAX as f64) as i8 as u8
            } else {
                scaled_unsigned(value, 128.0, u8::MAX as f64) as u8
            };
        }
        SampleFormat::I16 | SampleFormat::U16 => {
            let bytes = if format == SampleFormat::I16 {
                (scaled_signed(value, 32_768.0, i16::MIN as f64, i16::MAX as f64) as i16)
                    .to_le_bytes()
            } else {
                (scaled_unsigned(value, 32_768.0, u16::MAX as f64) as u16).to_le_bytes()
            };
            output[..2].copy_from_slice(&bytes);
        }
        SampleFormat::I32 | SampleFormat::U32 => {
            let bytes = if format == SampleFormat::I32 {
                (scaled_signed(value, 2_147_483_648.0, i32::MIN as f64, i32::MAX as f64) as i32)
                    .to_le_bytes()
            } else {
                (scaled_unsigned(value, 2_147_483_648.0, u32::MAX as f64) as u32).to_le_bytes()
            };
            output[..4].copy_from_slice(&bytes);
        }
        SampleFormat::F32 => output[..4].copy_from_slice(&value.to_le_bytes()),
        SampleFormat::F64 => output[..8].copy_from_slice(&f64::from(value).to_le_bytes()),
    }
}

fn scaled_signed(value: f32, scale: f64, min: f64, max: f64) -> f64 {
    (f64::from(value) * scale).round().clamp(min, max)
}

fn scaled_unsigned(value: f32, scale: f64, max: f64) -> f64 {
    f64::from(value)
        .mul_add(scale, scale)
        .round()
        .clamp(0.0, max)
}

/// Writes silence in `format` into whole frames of `out`.
pub(crate) fn encode_silence(format: DeviceFormat, out: &mut [u8]) {
    let sample_bytes = format.sample_format.sample_bytes();
    let silence = silence_bytes(format.sample_format);
    if format.channels == 0 || sample_bytes == 0 {
        out.fill(0);
        return;
    }
    let frame_bytes = format.frame_bytes();
    for frame in out.chunks_exact_mut(frame_bytes) {
        for channel in frame.chunks_exact_mut(sample_bytes) {
            channel.copy_from_slice(&silence[..sample_bytes]);
        }
    }
}

fn silence_bytes(format: SampleFormat) -> [u8; 8] {
    match format {
        SampleFormat::I8 => [0; 8],
        SampleFormat::U8 => [128; 8],
        SampleFormat::I16 | SampleFormat::I32 | SampleFormat::F32 => [0; 8],
        SampleFormat::U16 => [0, 128, 0, 128, 0, 128, 0, 128],
        SampleFormat::U32 => [0, 0, 0, 128, 0, 0, 0, 128],
        SampleFormat::F64 => [0; 8],
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const MONO_24K: SourceFormat = SourceFormat {
        sample_rate: 24_000,
        channels: 1,
    };
    const STEREO_24K: SourceFormat = SourceFormat {
        sample_rate: 24_000,
        channels: 2,
    };

    fn device(channels: u16, sample_format: SampleFormat) -> DeviceFormat {
        DeviceFormat {
            sample_rate: 24_000,
            channels,
            sample_format,
        }
    }

    fn pcm16(samples: &[i16]) -> Vec<u8> {
        samples.iter().flat_map(|s| s.to_le_bytes()).collect()
    }

    fn read_i16(bytes: &[u8]) -> Vec<i16> {
        bytes
            .as_chunks::<2>()
            .0
            .iter()
            .map(|c| i16::from_le_bytes(*c))
            .collect()
    }

    #[test]
    fn mono_i16_to_stereo_f32_duplicates_and_normalizes() {
        let converter = Converter::new(MONO_24K, device(2, SampleFormat::F32)).unwrap();
        let out = converter.convert(&pcm16(&[16_384, -16_384]));
        let values: Vec<f32> = out
            .as_chunks::<4>()
            .0
            .iter()
            .map(|c| f32::from_le_bytes(*c))
            .collect();
        assert_eq!(values, vec![0.5, 0.5, -0.5, -0.5]);
        assert_eq!(out.len(), 4 * 4);
    }

    #[test]
    fn mono_i16_to_stereo_u16_uses_signed_equilibrium() {
        let converter = Converter::new(MONO_24K, device(2, SampleFormat::U16)).unwrap();
        let out = converter.convert(&pcm16(&[0, 16_384, i16::MIN, i16::MAX]));
        let values: Vec<u16> = out
            .as_chunks::<2>()
            .0
            .iter()
            .map(|c| u16::from_le_bytes(*c))
            .collect();
        assert_eq!(
            values,
            vec![32_768, 32_768, 49_152, 49_152, 0, 0, 65_535, 65_535]
        );
    }

    #[test]
    fn other_encodings_round_trip_the_extremes() {
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
            let converter = Converter::new(MONO_24K, device(1, format)).unwrap();
            let out = converter.convert(&pcm16(&[i16::MIN, 0, i16::MAX]));
            assert_eq!(out.len(), 3 * format.sample_bytes(), "{format:?}");
            let silence = &out[format.sample_bytes()..2 * format.sample_bytes()];
            let expected = silence_bytes(format);
            assert_eq!(silence, &expected[..format.sample_bytes()], "{format:?}");
            // Extremes must not wrap around the encoding's range.
            let low = &out[..format.sample_bytes()];
            let high = &out[2 * format.sample_bytes()..];
            assert_ne!(low, silence, "{format:?}");
            assert_ne!(high, silence, "{format:?}");
        }
    }

    #[test]
    fn stereo_downmix_matches_audio_domain_average() {
        let converter = Converter::new(STEREO_24K, device(1, SampleFormat::I16)).unwrap();
        // audio-domain averages (16384 + 0) / 2 = 8192.
        let out = converter.convert(&pcm16(&[16_384, 0, 8_192, 8_192]));
        assert_eq!(read_i16(&out), vec![8_192, 8_192]);
    }

    #[test]
    fn channel_mapping_duplicates_up_and_truncates_down() {
        let converter = Converter::new(MONO_24K, device(3, SampleFormat::I16)).unwrap();
        assert_eq!(
            read_i16(&converter.convert(&pcm16(&[100]))),
            vec![100, 100, 100]
        );
        let converter = Converter::new(STEREO_24K, device(4, SampleFormat::I16)).unwrap();
        assert_eq!(
            read_i16(&converter.convert(&pcm16(&[100, 200]))),
            vec![100, 200, 200, 200]
        );
        let converter = Converter::new(STEREO_24K, device(1, SampleFormat::I16)).unwrap();
        assert_eq!(read_i16(&converter.convert(&pcm16(&[100, 200]))), vec![150]);
    }

    #[test]
    fn trailing_partial_frames_are_ignored() {
        let converter = Converter::new(MONO_24K, device(2, SampleFormat::F32)).unwrap();
        let mut input = pcm16(&[1, 2]);
        input.push(0xff);
        assert_eq!(converter.frames(&input), 2);
        assert_eq!(converter.convert(&input).len(), 2 * 8);
    }

    #[test]
    fn write_respects_a_shorter_output_buffer() {
        let converter = Converter::new(MONO_24K, device(2, SampleFormat::I16)).unwrap();
        let input = pcm16(&[1, 2, 3]);
        let mut out = vec![0u8; 4];
        assert_eq!(converter.write(&input, &mut out), 1);
        assert_eq!(read_i16(&out), vec![1, 1]);
    }

    #[test]
    fn silence_is_the_encoding_zero_point() {
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
            let device = device(2, format);
            let mut out = vec![0xffu8; 3 * device.frame_bytes()];
            encode_silence(device, &mut out);
            let expected = silence_bytes(format);
            for sample in out.chunks_exact(format.sample_bytes()) {
                assert_eq!(sample, &expected[..format.sample_bytes()], "{format:?}");
            }
        }
    }

    #[test]
    fn rate_mismatch_and_invalid_formats_are_rejected() {
        assert!(Converter::new(MONO_24K, device(2, SampleFormat::F32)).is_some());
        assert!(Converter::new(
            MONO_24K,
            DeviceFormat {
                sample_rate: 48_000,
                ..device(2, SampleFormat::F32)
            }
        )
        .is_none());
        assert!(Converter::new(
            SourceFormat {
                sample_rate: 0,
                channels: 1
            },
            device(2, SampleFormat::F32)
        )
        .is_none());
        assert!(Converter::new(
            MONO_24K,
            DeviceFormat {
                channels: 0,
                ..device(2, SampleFormat::F32)
            }
        )
        .is_none());
    }
}
