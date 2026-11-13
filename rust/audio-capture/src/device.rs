//! cpal-backed capture over the default host (ALSA on Linux).
//!
//! The structure follows `rust/audio-output/src/sink.rs`: hosts are selected
//! through the fallible `host_from_id`, the default host is tried first, and
//! the stream is opened with an explicit timeout so a wedged backend surfaces
//! as an error instead of blocking the caller. The cpal `pipewire` feature is
//! not used for the same reason as in the player: `libspa-sys` needs libclang
//! for bindgen, which the packaged build images do not ship; the ALSA default
//! host reaches PipeWire through the `pipewire-alsa` plugin.
//!
//! Every device callback only appends raw bytes to a ring behind a mutex: it
//! never decodes, resamples or logs. Conversion happens in
//! [`crate::Capture::pump`], on the caller's thread.

use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Duration;

use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};

use crate::base64;
use crate::{CaptureFactory, CaptureFormat, DeviceInfo, Error, Opened, SampleFormat, Source};

/// How long cpal may take to bring an input stream up before it is reported as
/// a start failure.
const OPEN_TIMEOUT: Duration = Duration::from_secs(2);

/// Most native audio one stream may buffer between drains.
///
/// The ring is drained by [`crate::Capture::pump`]. Capping it means a device
/// that keeps producing — or a caller that stops pumping — cannot grow memory
/// without limit; bytes beyond the cap are dropped, just as Qt's fixed 100 ms
/// source buffer dropped audio the reader was too slow to take. 4 MiB is over
/// 20 s of 48 kHz stereo float, far above any sane pump cadence, and matches
/// the C++ per-append block guard.
const MAX_BUFFERED: usize = 4 * 1024 * 1024;

/// Raw bytes captured by the stream callback.
#[derive(Debug, Default)]
struct Ring {
    bytes: VecDeque<u8>,
}

impl Ring {
    fn lock(ring: &Mutex<Ring>) -> MutexGuard<'_, Ring> {
        ring.lock().unwrap_or_else(|error| error.into_inner())
    }

    /// Appends what fits, discarding the rest.
    ///
    /// The ring is drained by [`crate::Capture::pump`]; capping it keeps a
    /// caller that stops pumping from growing memory without limit. Loss only
    /// happens to a caller that is not draining, which is already missing
    /// audio.
    fn push(&mut self, data: &[u8]) {
        let room = MAX_BUFFERED.saturating_sub(self.bytes.len());
        let accepted = room.min(data.len());
        self.bytes.extend(&data[..accepted]);
    }

    fn take(&mut self) -> Vec<u8> {
        self.bytes.drain(..).collect()
    }
}

/// cpal-backed factory over the default host.
#[derive(Debug, Default)]
pub struct CpalFactory;

/// Maps a cpal sample format onto one this crate decodes.
///
/// I24/U24 store 24 significant bits in four bytes and I64/U64/DSD are outside
/// what the voice-input normalizer accepted, so they are refused like the C++
/// `Reset` refused them.
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

/// Iterates the hosts like `sink.rs`: default first, the rest only when it
/// cannot provide an input device.
fn hosts() -> Vec<cpal::Host> {
    let default = cpal::default_host().id();
    let ordered = std::iter::once(default).chain(
        cpal::available_hosts()
            .into_iter()
            .filter(|h| *h != default),
    );
    ordered
        .filter_map(|id| cpal::host_from_id(id).ok())
        .collect()
}

/// Every input device of every host, with the default input flagged.
fn enumerate() -> Vec<(cpal::Device, bool, String)> {
    let mut found = Vec::new();
    for host in hosts() {
        let default = host.default_input_device();
        let Ok(devices) = host.input_devices() else {
            continue;
        };
        for device in devices {
            // A description is what the GUI shows; a device that cannot even
            // name itself is unusable, exactly like the C++ `description()`.
            let Ok(description) = device.description() else {
                continue;
            };
            let Ok(id) = device.id() else {
                continue;
            };
            let is_default = default
                .as_ref()
                .is_some_and(|d| d.id().ok() == Some(id.clone()));
            found.push((device, is_default, description.name().to_string()));
        }
    }
    found
}

/// Opens `device` (or the default input) at a format this crate can decode.
///
/// The requested 16 kHz mono PCM16 format is used when the device offers it;
/// otherwise its own default input configuration is taken. The stream rate is
/// never changed after the fact: the caller resamples what arrives.
fn open_device(device: Option<&str>, requested: CaptureFormat) -> Result<Opened, Error> {
    let devices = enumerate();
    let device = match device {
        Some(token) => {
            let Some((device, _, _)) = devices.iter().find(|(device, _, _)| {
                device
                    .id()
                    .is_ok_and(|id| base64::encode(id.to_string().as_bytes()) == token)
            }) else {
                // The named device is gone: no silent fallback to the default.
                return Err(Error::UNAVAILABLE);
            };
            device.clone()
        }
        None => {
            let Some((device, _, _)) = devices.iter().find(|(_, is_default, _)| *is_default) else {
                return Err(Error::UNAVAILABLE);
            };
            device.clone()
        }
    };
    let config = negotiate(&device, requested)?;
    let Some(encoding) = sample_format(config.sample_format()) else {
        return Err(Error::UNSUPPORTED_FORMAT);
    };
    if config.channels() == 0 || config.channels() > 32 {
        return Err(Error::UNSUPPORTED_FORMAT);
    }
    let format = CaptureFormat {
        sample_rate: config.sample_rate(),
        channels: config.channels(),
        sample_format: encoding,
    };
    let ring = Arc::new(Mutex::new(Ring::default()));
    let failed = Arc::new(AtomicBool::new(false));
    let callback_ring = Arc::clone(&ring);
    let callback_failed = Arc::clone(&failed);
    let data = move |data: &cpal::Data, _: &cpal::InputCallbackInfo| {
        Ring::lock(&callback_ring).push(data.bytes());
    };
    let errors = move |error: cpal::Error| {
        // An overrun means the callback did not read the device fast enough.
        // ALSA has already recovered the stream, and Qt's `QAudioSource` kept
        // running through the same condition, so it is not a capture failure.
        // Everything else (device removed, stream invalidated) is terminal.
        if error.kind() != cpal::ErrorKind::Xrun {
            callback_failed.store(true, Ordering::Relaxed);
        }
    };
    let stream = device
        .build_input_stream_raw(
            config.config(),
            cpal_format(encoding),
            data,
            errors,
            Some(OPEN_TIMEOUT),
        )
        .map_err(|_| Error::START)?;
    stream.play().map_err(|_| Error::START)?;
    Ok(Opened {
        format,
        source: Box::new(CpalSource {
            _stream: stream,
            format,
            ring,
            failed,
        }),
    })
}

/// Picks the requested format when offered, else the device default.
fn negotiate(
    device: &cpal::Device,
    requested: CaptureFormat,
) -> Result<cpal::SupportedStreamConfig, Error> {
    if let Ok(configs) = device.supported_input_configs() {
        let mut best: Option<cpal::SupportedStreamConfig> = None;
        for range in configs {
            let Some(encoding) = sample_format(range.sample_format()) else {
                continue;
            };
            if encoding != requested.sample_format || range.channels() != requested.channels {
                continue;
            }
            let Some(config) = range.try_with_sample_rate(requested.sample_rate) else {
                continue;
            };
            // Prefer a buffer range the backend reports, else keep the first.
            if matches!(
                config.buffer_size(),
                cpal::SupportedBufferSize::Range { .. }
            ) {
                best = Some(config);
                break;
            }
            best.get_or_insert(config);
        }
        if let Some(config) = best {
            return Ok(config);
        }
    }
    device
        .default_input_config()
        .map_err(|_| Error::UNAVAILABLE)
}

struct CpalSource {
    /// Kept alive for capture; dropping it stops and closes the stream.
    _stream: cpal::Stream,
    format: CaptureFormat,
    ring: Arc<Mutex<Ring>>,
    failed: Arc<AtomicBool>,
}

impl std::fmt::Debug for CpalSource {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("CpalSource")
            .field("format", &self.format)
            .finish_non_exhaustive()
    }
}

impl Source for CpalSource {
    fn format(&self) -> CaptureFormat {
        self.format
    }

    fn take(&mut self) -> Vec<u8> {
        Ring::lock(&self.ring).take()
    }

    fn error(&self) -> Option<Error> {
        self.failed.load(Ordering::Relaxed).then_some(Error::DEVICE)
    }
}

impl CaptureFactory for CpalFactory {
    fn devices(&mut self) -> Vec<DeviceInfo> {
        enumerate()
            .into_iter()
            .map(|(device, is_default, name)| DeviceInfo {
                id: device
                    .id()
                    .map(|id| base64::encode(id.to_string().as_bytes()))
                    .unwrap_or_default(),
                name,
                is_default,
            })
            .filter(|info| !info.id.is_empty())
            .collect()
    }

    fn open(&mut self, device: Option<&str>, requested: CaptureFormat) -> Result<Opened, Error> {
        open_device(device, requested)
    }
}
