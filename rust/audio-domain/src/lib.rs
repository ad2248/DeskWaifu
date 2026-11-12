//! Standalone PCM normalization and WAV encoding primitives.

/// Supported native-endian input sample formats.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SampleFormat {
    UInt8,
    Int16,
    Int32,
    Float32,
}

impl SampleFormat {
    fn bytes_per_sample(self) -> usize {
        match self {
            Self::UInt8 => 1,
            Self::Int16 => 2,
            Self::Int32 | Self::Float32 => 4,
        }
    }
}

/// Convert interleaved native-format samples to mono little-endian PCM16.
///
/// Integer and float decoding follows the voice-input normalizer. Non-finite
/// float samples contribute zero to the channel average; resulting values are
/// clamped to [-1, 1] and quantized using round-to-nearest.
pub fn normalize_pcm(
    input: &[u8],
    format: SampleFormat,
    channels: usize,
) -> Result<Vec<u8>, &'static str> {
    if channels == 0 || channels > 32 {
        return Err("Invalid channel count");
    }
    let frame_size = format.bytes_per_sample() * channels;
    if !input.len().is_multiple_of(frame_size) {
        return Err("Invalid audio sample buffer length");
    }
    let frames = input.len() / frame_size;
    if frames > 300 * 16_000 {
        return Err("Audio buffer limit exceeded");
    }

    let sample_size = format.bytes_per_sample();
    let mut output = Vec::with_capacity(frames * 2);
    for frame in input.chunks_exact(frame_size) {
        let mut sum = 0.0f64;
        for sample in frame.chunks_exact(sample_size) {
            let value = match format {
                SampleFormat::UInt8 => (f64::from(sample[0]) - 128.0) / 128.0,
                SampleFormat::Int16 => {
                    f64::from(i16::from_ne_bytes([sample[0], sample[1]])) / 32768.0
                }
                SampleFormat::Int32 => {
                    f64::from(i32::from_ne_bytes(
                        sample.try_into().expect("sample width checked"),
                    )) / 2_147_483_648.0
                }
                SampleFormat::Float32 => {
                    let value =
                        f32::from_ne_bytes(sample.try_into().expect("sample width checked"));
                    if value.is_finite() {
                        f64::from(value)
                    } else {
                        0.0
                    }
                }
            };
            // The source decodes each channel to float before summing in double.
            sum += f64::from(value as f32);
        }
        let average = (sum / channels as f64) as f32;
        let average = if average.is_finite() {
            average.clamp(-1.0, 1.0)
        } else {
            0.0
        };
        let quantized = (average * 32768.0).round().clamp(-32768.0, 32767.0) as i16;
        output.extend_from_slice(&quantized.to_le_bytes());
    }
    Ok(output)
}

/// Encode 16 kHz mono PCM16 little-endian data as WAV, appending silence.
pub fn encode_wav(pcm: &[u8], trailing_silence_ms: i32) -> Result<Vec<u8>, &'static str> {
    if !pcm.len().is_multiple_of(2)
        || pcm.len() > 300 * 32_000
        || !(0..=1000).contains(&trailing_silence_ms)
    {
        return Err("Invalid PCM audio");
    }
    let silence_bytes = trailing_silence_ms as usize * 32;
    let data_size = pcm.len() + silence_bytes;
    let mut wav = Vec::with_capacity(44 + data_size);
    wav.extend_from_slice(b"RIFF");
    wav.extend_from_slice(&(36u32 + data_size as u32).to_le_bytes());
    wav.extend_from_slice(b"WAVEfmt ");
    wav.extend_from_slice(&16u32.to_le_bytes());
    wav.extend_from_slice(&1u16.to_le_bytes());
    wav.extend_from_slice(&1u16.to_le_bytes());
    wav.extend_from_slice(&16_000u32.to_le_bytes());
    wav.extend_from_slice(&32_000u32.to_le_bytes());
    wav.extend_from_slice(&2u16.to_le_bytes());
    wav.extend_from_slice(&16u16.to_le_bytes());
    wav.extend_from_slice(b"data");
    wav.extend_from_slice(&(data_size as u32).to_le_bytes());
    wav.extend_from_slice(pcm);
    wav.resize(44 + data_size, 0);
    Ok(wav)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn native_bytes<T: Copy>(values: &[T]) -> Vec<u8> {
        // Test fixtures are represented using the current native byte order,
        // matching the source decoder's memcpy behavior.
        let len = std::mem::size_of_val(values);
        // SAFETY: values is a valid initialized slice and is only read for len bytes.
        unsafe { std::slice::from_raw_parts(values.as_ptr().cast::<u8>(), len).to_vec() }
    }

    #[test]
    fn normalizes_native_formats_and_averages_channels() {
        let fixtures = [
            (SampleFormat::UInt8, vec![0xc0, 0x80]),
            (SampleFormat::Int16, native_bytes(&[16384i16, 0])),
            (SampleFormat::Int32, native_bytes(&[1073741824i32, 0])),
            (SampleFormat::Float32, native_bytes(&[0.5f32, 0.0])),
        ];
        for (format, input) in fixtures {
            assert_eq!(normalize_pcm(&input, format, 2).unwrap(), [0x00, 0x20]);
        }
    }

    #[test]
    fn passthrough_clamps_and_nonfinite_becomes_zero() {
        let input = native_bytes(&[i16::MIN, -1, 0, i16::MAX]);
        assert_eq!(
            normalize_pcm(&input, SampleFormat::Int16, 1).unwrap(),
            input
        );
        let floats = native_bytes(&[f32::INFINITY, f32::NAN, -2.0, 2.0]);
        assert_eq!(
            normalize_pcm(&floats, SampleFormat::Float32, 1).unwrap(),
            [0, 0, 0, 0, 0, 128, 255, 127]
        );
    }

    #[test]
    fn rejects_invalid_channel_counts_lengths_and_input_boundary() {
        assert!(normalize_pcm(&[], SampleFormat::Int16, 0).is_err());
        assert!(normalize_pcm(&[], SampleFormat::Int16, 33).is_err());
        assert!(normalize_pcm(&[0], SampleFormat::Int16, 1).is_err());
        assert!(normalize_pcm(&[0; 3], SampleFormat::Int16, 2).is_err());
        assert!(normalize_pcm(&vec![0; 300 * 32_000 + 2], SampleFormat::Int16, 1).is_err());
        assert_eq!(
            normalize_pcm(&vec![0; 300 * 32_000], SampleFormat::Int16, 1)
                .unwrap()
                .len(),
            300 * 32_000
        );
    }

    #[test]
    fn wav_header_payload_and_silence_match_source() {
        let pcm = [0x00, 0x80, 0xff, 0x7f, 1, 0];
        let wav = encode_wav(&pcm, 1).unwrap();
        assert_eq!(wav.len(), 44 + pcm.len() + 32);
        assert_eq!(&wav[..4], b"RIFF");
        assert_eq!(&wav[8..16], b"WAVEfmt ");
        assert_eq!(
            u32::from_le_bytes(wav[4..8].try_into().unwrap()),
            (wav.len() - 8) as u32
        );
        assert_eq!(u16::from_le_bytes(wav[20..22].try_into().unwrap()), 1);
        assert_eq!(u16::from_le_bytes(wav[22..24].try_into().unwrap()), 1);
        assert_eq!(u32::from_le_bytes(wav[24..28].try_into().unwrap()), 16000);
        assert_eq!(u32::from_le_bytes(wav[28..32].try_into().unwrap()), 32000);
        assert_eq!(u16::from_le_bytes(wav[32..34].try_into().unwrap()), 2);
        assert_eq!(u16::from_le_bytes(wav[34..36].try_into().unwrap()), 16);
        assert_eq!(&wav[36..40], b"data");
        assert_eq!(
            u32::from_le_bytes(wav[40..44].try_into().unwrap()),
            (pcm.len() + 32) as u32
        );
        assert_eq!(&wav[44..44 + pcm.len()], &pcm);
        assert_eq!(&wav[44 + pcm.len()..], &[0; 32]);
        assert_eq!(encode_wav(&[], 0).unwrap().len(), 44);
        assert_eq!(encode_wav(&[], 1000).unwrap().len(), 44 + 32_000);
    }

    #[test]
    fn wav_rejects_odd_oversized_and_invalid_silence() {
        assert_eq!(encode_wav(&[0], 0), Err("Invalid PCM audio"));
        assert_eq!(
            encode_wav(&vec![0; 300 * 32_000 + 2], 0),
            Err("Invalid PCM audio")
        );
        assert_eq!(encode_wav(&[], -1), Err("Invalid PCM audio"));
        assert_eq!(encode_wav(&[], 1001), Err("Invalid PCM audio"));
        assert_eq!(
            encode_wav(&vec![0; 300 * 32_000], 0).unwrap().len(),
            44 + 300 * 32_000
        );
    }
}
