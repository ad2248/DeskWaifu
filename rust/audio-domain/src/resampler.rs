//! Incremental mono PCM16 resampling to 16 kHz using system libsamplerate.
//!
//! Linking requires the native `samplerate` library (`#[link(name = "samplerate")]`).
//! Input is interleaved float32; non-finite samples contribute zero during downmix.

use std::ptr;

const OUTPUT_RATE: f64 = 16_000.0;
const MAX_OUTPUT_FRAMES: usize = 300 * 16_000;
const SRC_SINC_FASTEST: i32 = 2;

#[repr(C)]
struct SrcData {
    data_in: *const f32,
    data_out: *mut f32,
    input_frames: std::ffi::c_long,
    output_frames: std::ffi::c_long,
    input_frames_used: std::ffi::c_long,
    output_frames_gen: std::ffi::c_long,
    end_of_input: i32,
    src_ratio: f64,
}

#[link(name = "samplerate")]
unsafe extern "C" {
    fn src_new(converter_type: i32, channels: i32, error: *mut i32) -> *mut SrcState;
    fn src_process(state: *mut SrcState, data: *mut SrcData) -> i32;
    fn src_delete(state: *mut SrcState) -> *mut SrcState;
}

#[repr(C)]
struct SrcState {
    _private: [u8; 0],
}

/// Incrementally downmixes interleaved float32 audio and resamples it to 16 kHz PCM16.
pub struct Resampler {
    state: *mut SrcState,
    passthrough: bool,
    ratio: f64,
    channels: usize,
    finished: bool,
    output_frames: usize,
}

impl Resampler {
    /// Create a resampler for a positive input sample rate and 1–32 channels.
    pub fn new(input_rate: i32, channels: usize) -> Result<Self, &'static str> {
        if input_rate <= 0 || channels == 0 || channels > 32 || channels > i32::MAX as usize {
            return Err("Invalid resampler format");
        }
        let ratio = OUTPUT_RATE / f64::from(input_rate);
        let passthrough = input_rate == OUTPUT_RATE as i32;
        let mut error = 0;
        // SAFETY: arguments are validated; libsamplerate owns the returned state.
        let state = if passthrough {
            ptr::null_mut()
        } else {
            unsafe { src_new(SRC_SINC_FASTEST, 1, &mut error) }
        };
        if !passthrough && state.is_null() {
            return Err("Unable to create resampler");
        }
        Ok(Self {
            state,
            passthrough,
            ratio,
            channels,
            finished: false,
            output_frames: 0,
        })
    }

    /// Process another interleaved chunk and return newly generated little-endian PCM16.
    pub fn append(&mut self, input: &[f32]) -> Result<Vec<u8>, &'static str> {
        if self.finished || !input.len().is_multiple_of(self.channels) {
            return Err("Invalid resampler input");
        }
        let mono = downmix(input, self.channels);
        if self.passthrough {
            return self.convert_samples(&mono);
        }
        self.process(&mono, false)
    }

    /// Flush the filter tail. May be called only once; call `reset` to reuse the wrapper.
    pub fn finish(&mut self) -> Result<Vec<u8>, &'static str> {
        if self.finished {
            return Err("Resampler already finished");
        }
        self.finished = true;
        if self.passthrough {
            return Ok(Vec::new());
        }
        self.process(&[], true)
    }

    /// Reset filter history and output accounting, retaining the same input format.
    pub fn reset(&mut self) -> Result<(), &'static str> {
        // Recreate the state so reset has the same semantics as a fresh converter.
        // SAFETY: state came from src_new and is released exactly once here.
        if !self.passthrough {
            unsafe {
                src_delete(self.state);
            }
            let mut error = 0;
            // SAFETY: fixed supported converter and mono channel count.
            self.state = unsafe { src_new(SRC_SINC_FASTEST, 1, &mut error) };
            if self.state.is_null() {
                return Err("Unable to reset resampler");
            }
        }
        self.finished = false;
        self.output_frames = 0;
        Ok(())
    }

    fn convert_samples(&mut self, samples: &[f32]) -> Result<Vec<u8>, &'static str> {
        if self.output_frames + samples.len() > MAX_OUTPUT_FRAMES {
            return Err("Resampler output limit exceeded");
        }
        self.output_frames += samples.len();
        let mut output = Vec::with_capacity(samples.len() * 2);
        for sample in samples {
            let value = if sample.is_finite() {
                sample.clamp(-1.0, 1.0)
            } else {
                0.0
            };
            let pcm = (value * 32768.0).round().clamp(-32768.0, 32767.0) as i16;
            output.extend_from_slice(&pcm.to_le_bytes());
        }
        Ok(output)
    }

    fn process(&mut self, input: &[f32], end: bool) -> Result<Vec<u8>, &'static str> {
        let mut consumed = 0usize;
        let mut output = Vec::new();
        loop {
            let remaining = input.len() - consumed;
            // Keep output space ample even during final filter flushing.
            let capacity = (remaining as f64 * self.ratio).ceil() as usize + 4096;
            let mut samples = vec![0.0f32; capacity.max(1)];
            let mut data = SrcData {
                // Even when input_frames is zero, pass a non-null aligned pointer;
                // libsamplerate's final drain mirrors the native converter contract.
                data_in: input[consumed..].as_ptr(),
                data_out: samples.as_mut_ptr(),
                input_frames: remaining as std::ffi::c_long,
                output_frames: samples.len() as std::ffi::c_long,
                input_frames_used: 0,
                output_frames_gen: 0,
                end_of_input: i32::from(end),
                src_ratio: self.ratio,
            };
            // SAFETY: pointers reference valid slices for the frame counts supplied.
            let error = unsafe { src_process(self.state, &mut data) };
            if error != 0 {
                return Err("Resampler processing failed");
            }
            let used = data.input_frames_used as usize;
            let generated = data.output_frames_gen as usize;
            consumed += used;
            if self.output_frames + generated > MAX_OUTPUT_FRAMES {
                return Err("Resampler output limit exceeded");
            }
            self.output_frames += generated;
            for sample in &samples[..generated] {
                let value = if sample.is_finite() {
                    sample.clamp(-1.0, 1.0)
                } else {
                    0.0
                };
                let pcm = (value * 32768.0).round().clamp(-32768.0, 32767.0) as i16;
                output.extend_from_slice(&pcm.to_le_bytes());
            }
            if consumed == input.len() && (!end || (used == 0 && generated == 0)) {
                break;
            }
            // For end-of-input, continue until the converter drains its filter tail.
            if used == 0 && generated == 0 {
                return Err("Resampler stalled");
            }
        }
        Ok(output)
    }
}

impl Drop for Resampler {
    fn drop(&mut self) {
        if !self.passthrough && !self.state.is_null() {
            // SAFETY: state is owned by this wrapper and deleted exactly once.
            unsafe {
                src_delete(self.state);
            }
        }
    }
}

fn downmix(input: &[f32], channels: usize) -> Vec<f32> {
    input
        .chunks_exact(channels)
        .map(|frame| {
            let sum: f64 = frame
                .iter()
                .map(|&sample| {
                    if sample.is_finite() {
                        f64::from(sample)
                    } else {
                        0.0
                    }
                })
                .sum();
            let value = (sum / channels as f64) as f32;
            if value.is_finite() {
                value.clamp(-1.0, 1.0)
            } else {
                0.0
            }
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixture() -> Vec<f32> {
        (0..48_000)
            .flat_map(|i| {
                let sample = (std::f32::consts::TAU * 440.0 * i as f32 / 48_000.0).sin() * 0.6;
                [sample, sample]
            })
            .collect()
    }

    fn convert(chunks: &[usize]) -> Vec<u8> {
        let input = fixture();
        let mut resampler = Resampler::new(48_000, 2).unwrap();
        let mut out = Vec::new();
        let mut frame = 0;
        for &size in chunks {
            let end = (frame + size).min(input.len() / 2);
            out.extend(resampler.append(&input[frame * 2..end * 2]).unwrap());
            frame = end;
        }
        if frame < input.len() / 2 {
            out.extend(resampler.append(&input[frame * 2..]).unwrap());
        }
        out.extend(resampler.finish().unwrap());
        out
    }

    #[test]
    fn chunking_is_invariant_and_tail_is_flushed() {
        let full = convert(&[48_000]);
        let chunked = convert(&vec![137; 351]);
        assert_eq!(full, chunked);
        assert_eq!(full.len(), 32_000);
        assert!(full[full.len() - 64..].iter().any(|&byte| byte != 0));
    }

    #[test]
    fn reset_finish_and_limits() {
        assert!(Resampler::new(0, 1).is_err());
        assert!(Resampler::new(48_000, 0).is_err());
        let mut resampler = Resampler::new(16_000, 1).unwrap();
        let first = resampler.append(&[0.25; 16_000]).unwrap();
        assert_eq!(first.len(), 32_000);
        assert!(resampler.finish().unwrap().is_empty());
        assert!(resampler.finish().is_err());
        resampler.reset().unwrap();
        assert_eq!(resampler.append(&[0.0; 16_000]).unwrap().len(), 32_000);
        resampler.finish().unwrap();
        resampler.reset().unwrap();
        let output = resampler.append(&vec![0.0; 300 * 16_000]).unwrap();
        assert_eq!(output.len(), 300 * 16_000 * 2);
        assert!(resampler.append(&[0.0]).is_err());
    }
}
