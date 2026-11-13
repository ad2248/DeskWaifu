//! Incremental native-format to 16 kHz mono PCM16 little-endian conversion.
//!
//! Port of `plugins/voice_input/private/audio/PcmNormalizer.cpp`. The resampler
//! and the channel downmix are `deskwaifu_audio_domain::resampler::Resampler`,
//! the same `SRC_SINC_FASTEST` libsamplerate state machine and averaging rule
//! the C++ normalizer drives directly; this module owns only the frame
//! assembly, the byte-order handling and the 16 kHz mono PCM16 passthrough.
//!
//! The C++ object was fed native-format bytes, so this one is too. Decoding
//! matches `deskwaifu_audio_domain::normalize_pcm` for the sample encodings it
//! shares, with two additions the chunked path needs: signed 8-bit, unsigned
//! 16-bit, unsigned 32-bit and `f64` are accepted because cpal can deliver
//! them, and a 16 kHz mono PCM16 stream is copied through without decoding at
//! all, as the C++ `Append` fast path did.

use deskwaifu_audio_domain::resampler::Resampler;

use crate::{CaptureFormat, Error, SampleFormat, MAX_PCM_BYTES};

/// Largest single `append` call, mirroring the C++ guard.
const MAX_INPUT_BLOCK: usize = 4 * 1024 * 1024;

/// Frames decoded per internal step, bounding temporary allocations.
const STEP_FRAMES: usize = 4096;

/// Converter from a device format to 16 kHz mono PCM16.
pub struct Normalizer {
    format: CaptureFormat,
    /// `None` for the 16 kHz mono PCM16 passthrough.
    resampler: Option<Resampler>,
    pending: Vec<u8>,
    output: Vec<u8>,
    active: bool,
}

impl std::fmt::Debug for Normalizer {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Normalizer")
            .field("format", &self.format)
            .field("passthrough", &self.resampler.is_none())
            .field("pending", &self.pending.len())
            .field("output", &self.output.len())
            .field("active", &self.active)
            .finish()
    }
}

impl Normalizer {
    /// Creates a converter, rejecting a format the C++ `Reset` rejected.
    pub fn new(format: CaptureFormat) -> Result<Self, Error> {
        if !format.is_valid() {
            return Err(Error::UNSUPPORTED_FORMAT);
        }
        let passthrough = format.sample_rate == 16_000
            && format.channels == 1
            && format.sample_format == SampleFormat::I16;
        Self::assemble(format, passthrough)
    }

    /// Appends native-format bytes.
    pub fn append(&mut self, bytes: &[u8]) -> Result<(), Error> {
        if !self.active {
            return Err(Error::INACTIVE);
        }
        if bytes.len() > MAX_INPUT_BLOCK {
            return Err(Error::INPUT_TOO_LARGE);
        }
        self.pending.extend_from_slice(bytes);
        let frame_size = self.format.frame_bytes();
        let complete = self.pending.len() / frame_size * frame_size;
        if self.resampler.is_none() {
            if complete > MAX_PCM_BYTES - self.output.len() {
                return Err(Error::BUFFER_LIMIT);
            }
            self.output.extend_from_slice(&self.pending[..complete]);
        } else {
            let sample_size = self.format.sample_format.sample_bytes();
            let mut offset = 0;
            let mut float = Vec::with_capacity(STEP_FRAMES * usize::from(self.format.channels));
            while offset < complete {
                let frames = STEP_FRAMES.min((complete - offset) / frame_size);
                float.clear();
                float.extend(
                    self.pending[offset..offset + frames * frame_size]
                        .chunks_exact(sample_size)
                        .map(|sample| decode(sample, self.format.sample_format)),
                );
                self.convert(&float)?;
                offset += frames * frame_size;
            }
        }
        self.pending.drain(..complete);
        Ok(())
    }

    /// Flushes the resampler tail and returns the complete PCM.
    pub fn finish(&mut self) -> Result<Vec<u8>, Error> {
        if !self.active || !self.pending.is_empty() {
            return Err(Error::INCOMPLETE_FRAME);
        }
        if let Some(resampler) = &mut self.resampler {
            let tail = resampler.finish().map_err(resample_error)?;
            if tail.len() > MAX_PCM_BYTES - self.output.len() {
                return Err(Error::BUFFER_LIMIT);
            }
            self.output.extend_from_slice(&tail);
        }
        self.active = false;
        Ok(std::mem::take(&mut self.output))
    }

    /// Drops every buffer and deactivates the converter.
    pub fn clear(&mut self) {
        self.pending.clear();
        self.output.clear();
        self.active = false;
    }

    /// Feeds interleaved float samples to the resampler.
    ///
    /// `audio-domain`'s `Resampler` performs the channel downmix itself, with
    /// the same averaging rule and clamping as `normalize_pcm`, so this crate
    /// does not downmix first.
    fn convert(&mut self, samples: &[f32]) -> Result<(), Error> {
        let Some(resampler) = &mut self.resampler else {
            return Ok(());
        };
        let pcm = resampler.append(samples).map_err(resample_error)?;
        // The wrapper enforces its own 300 s ceiling; this one keeps the
        // assembled buffer at the exact C++ `MaxPcmBytes` boundary.
        if pcm.len() > MAX_PCM_BYTES - self.output.len() {
            return Err(Error::BUFFER_LIMIT);
        }
        self.output.extend_from_slice(&pcm);
        Ok(())
    }

    fn assemble(format: CaptureFormat, passthrough: bool) -> Result<Self, Error> {
        if passthrough {
            return Ok(Self::assemble_with(format, None));
        }
        let resampler = Resampler::new(format.sample_rate as i32, usize::from(format.channels))
            .map_err(|_| Error::RESAMPLE_INIT)?;
        Ok(Self::assemble_with(format, Some(resampler)))
    }

    fn assemble_with(format: CaptureFormat, resampler: Option<Resampler>) -> Self {
        Self {
            format,
            resampler,
            pending: Vec::new(),
            output: Vec::new(),
            active: true,
        }
    }
}

fn resample_error(_: &'static str) -> Error {
    Error::RESAMPLE_FAILED
}

/// Decodes one native-format sample to `f32` in `[-1, 1]`.
///
/// Integer scaling and the non-finite handling match
/// `deskwaifu_audio_domain::normalize_pcm`; `f64` is rounded through `f32`
/// because the resampler takes `f32`, the C++ normalizer's only sample type.
fn decode(sample: &[u8], format: SampleFormat) -> f32 {
    let value = match format {
        SampleFormat::I8 => f64::from(sample[0] as i8) / 128.0,
        SampleFormat::U8 => (f64::from(sample[0]) - 128.0) / 128.0,
        SampleFormat::I16 => f64::from(i16::from_ne_bytes([sample[0], sample[1]])) / 32768.0,
        SampleFormat::U16 => {
            (f64::from(u16::from_ne_bytes([sample[0], sample[1]])) - 32768.0) / 32768.0
        }
        SampleFormat::I32 => {
            f64::from(i32::from_ne_bytes([
                sample[0], sample[1], sample[2], sample[3],
            ])) / 2_147_483_648.0
        }
        SampleFormat::U32 => {
            (f64::from(u32::from_ne_bytes([
                sample[0], sample[1], sample[2], sample[3],
            ])) - 2_147_483_648.0)
                / 2_147_483_648.0
        }
        SampleFormat::F32 => {
            let value = f32::from_ne_bytes([sample[0], sample[1], sample[2], sample[3]]);
            if value.is_finite() {
                f64::from(value)
            } else {
                0.0
            }
        }
        SampleFormat::F64 => {
            let value = f64::from_ne_bytes([
                sample[0], sample[1], sample[2], sample[3], sample[4], sample[5], sample[6],
                sample[7],
            ]);
            if value.is_finite() {
                value
            } else {
                0.0
            }
        }
    };
    value as f32
}

#[cfg(test)]
mod tests {
    use super::*;

    fn format(rate: u32, channels: u16, sample_format: SampleFormat) -> CaptureFormat {
        CaptureFormat {
            sample_rate: rate,
            channels,
            sample_format,
        }
    }

    fn native<T: Copy>(values: &[T]) -> Vec<u8> {
        // SAFETY: the slice is initialized and only read for its own length.
        unsafe {
            std::slice::from_raw_parts(values.as_ptr().cast::<u8>(), std::mem::size_of_val(values))
                .to_vec()
        }
    }

    /// `PcmNormalizer::passthroughAndTail` from `AudioTest.cpp`.
    #[test]
    fn passthrough_copies_bytes_and_rejects_a_partial_frame() {
        let mut normalizer = Normalizer::new(format(16_000, 1, SampleFormat::I16)).unwrap();
        let input = [0x00u8, 0x80, 0xff, 0xff, 0x00, 0x00, 0x01, 0x00, 0xff, 0x7f];
        normalizer.append(&input[..3]).unwrap();
        normalizer.append(&input[3..]).unwrap();
        assert_eq!(normalizer.finish().unwrap(), input);

        let mut normalizer = Normalizer::new(format(16_000, 1, SampleFormat::I16)).unwrap();
        normalizer.append(&[0]).unwrap();
        assert_eq!(normalizer.finish().unwrap_err(), Error::INCOMPLETE_FRAME);
    }

    /// `PcmNormalizer::nativeFormats` from `AudioTest.cpp`: each native
    /// encoding must reach the same PCM through the downmix.
    #[test]
    fn native_formats_downmix_to_the_same_pcm() {
        let fixtures = [
            (SampleFormat::U8, vec![0xc0u8, 0x80]),
            (SampleFormat::I16, native(&[16384i16, 0])),
            (SampleFormat::I32, native(&[1073741824i32, 0])),
            (SampleFormat::F32, native(&[0.5f32, 0.0])),
        ];
        for (sample, input) in fixtures {
            let mut normalizer = Normalizer::new(format(16_000, 2, sample)).unwrap();
            // The C++ test appended one byte per call; do the same.
            for byte in input {
                normalizer.append(&[byte]).unwrap();
            }
            assert_eq!(normalizer.finish().unwrap(), [0x00, 0x20], "{sample:?}");
        }
    }

    /// `PcmNormalizer::resamplingIsChunkInvariantAndFlushesTail`.
    #[test]
    fn resampling_is_chunk_invariant_and_flushes_the_tail() {
        let input: Vec<f32> = (0..48_000)
            .flat_map(|i| {
                let sample = 0.4 * (std::f32::consts::TAU * 440.0 * i as f32 / 48_000.0).sin();
                [sample, sample]
            })
            .collect();
        let bytes = native(&input);
        let convert = |chunk: usize| {
            let mut normalizer = Normalizer::new(format(48_000, 2, SampleFormat::F32)).unwrap();
            for block in bytes.chunks(chunk) {
                normalizer.append(block).unwrap();
            }
            normalizer.finish().unwrap()
        };
        let whole = convert(bytes.len());
        assert_eq!(whole.len(), 32_000);
        assert_eq!(convert(137), whole);
        // The recorded signal reaches the end, not just an implicit zero tail.
        assert!(whole[whole.len() - 100..].iter().any(|&byte| byte != 0));
    }

    /// `PcmNormalizer::limitsAndInvalidFormat`.
    #[test]
    fn limits_and_invalid_formats() {
        assert_eq!(
            Normalizer::new(CaptureFormat {
                sample_rate: 0,
                ..CaptureFormat::REQUESTED
            })
            .unwrap_err(),
            Error::UNSUPPORTED_FORMAT
        );
        assert_eq!(
            Normalizer::new(CaptureFormat {
                channels: 33,
                ..CaptureFormat::REQUESTED
            })
            .unwrap_err(),
            Error::UNSUPPORTED_FORMAT
        );
        assert_eq!(
            Normalizer::new(CaptureFormat {
                sample_rate: 384_001,
                ..CaptureFormat::REQUESTED
            })
            .unwrap_err(),
            Error::UNSUPPORTED_FORMAT
        );
        let mut normalizer = Normalizer::new(CaptureFormat::REQUESTED).unwrap();
        assert_eq!(
            normalizer
                .append(&vec![0; 4 * 1024 * 1024 + 1])
                .unwrap_err(),
            Error::INPUT_TOO_LARGE
        );

        let mut normalizer = Normalizer::new(CaptureFormat::REQUESTED).unwrap();
        for _ in 0..300 {
            normalizer.append(&vec![0; 32_000]).unwrap();
        }
        assert_eq!(normalizer.append(&[0, 0]).unwrap_err(), Error::BUFFER_LIMIT);

        let mut normalizer = Normalizer::new(CaptureFormat::REQUESTED).unwrap();
        normalizer.clear();
        assert_eq!(normalizer.append(&[]).unwrap_err(), Error::INACTIVE);
    }

    /// Integer scaling and non-finite handling must match
    /// `deskwaifu_audio_domain::normalize_pcm`, because the resampler only ever
    /// sees the values this function produces.
    #[test]
    fn decoding_matches_audio_domain_normalize_pcm() {
        use deskwaifu_audio_domain::{normalize_pcm, SampleFormat as DomainFormat};

        let cases = [
            (
                SampleFormat::U8,
                DomainFormat::UInt8,
                vec![0x00u8, 0x40, 0x80, 0xc0, 0xff],
            ),
            (
                SampleFormat::I16,
                DomainFormat::Int16,
                native(&[i16::MIN, -1, 0, 16384, i16::MAX]),
            ),
            (
                SampleFormat::I32,
                DomainFormat::Int32,
                native(&[i32::MIN, -1, 0, 1_073_741_824, i32::MAX]),
            ),
            (
                SampleFormat::F32,
                DomainFormat::Float32,
                native(&[f32::NEG_INFINITY, f32::NAN, -1.0, 0.0, 0.5]),
            ),
        ];
        for (ours, domain, input) in cases {
            let mut normalizer = Normalizer::new(format(16_000, 1, ours)).unwrap();
            normalizer.append(&input).unwrap();
            let converted = normalizer.finish().unwrap();
            // `normalize_pcm` writes the same mono PCM16 the passthrough path
            // needs, so compare the decoded samples, not the raw bytes: the
            // resampler may round the last bit differently.
            let expected = normalize_pcm(&input, domain, 1).unwrap();
            assert_eq!(converted.len(), expected.len(), "{ours:?}");
            for (a, b) in converted.chunks(2).zip(expected.chunks(2)) {
                let a = i16::from_le_bytes([a[0], a[1]]) as i32;
                let b = i16::from_le_bytes([b[0], b[1]]) as i32;
                assert!((a - b).abs() <= 1, "{ours:?}: {a} vs {b}");
            }
        }
        // Byte-for-byte equality holds for the passthrough format.
        let input = native(&[i16::MIN, -1, 0, i16::MAX]);
        assert_eq!(
            normalize_pcm(&input, DomainFormat::Int16, 1).unwrap(),
            input
        );
    }

    #[test]
    fn f64_and_wide_integer_encodings_are_accepted() {
        for (sample, input, expected) in [
            (SampleFormat::I8, native(&[64i8]), Some(16_384i16)),
            (SampleFormat::U16, native(&[49152u16]), Some(16_384)),
            (SampleFormat::U32, native(&[3_221_225_472u32]), Some(16_384)),
            (SampleFormat::F64, native(&[0.5f64]), Some(16_384)),
        ] {
            let mut normalizer = Normalizer::new(format(16_000, 1, sample)).unwrap();
            normalizer.append(&input).unwrap();
            let pcm = normalizer.finish().unwrap();
            assert_eq!(
                i16::from_le_bytes([pcm[0], pcm[1]]),
                expected.unwrap(),
                "{sample:?}"
            );
        }
    }
}
