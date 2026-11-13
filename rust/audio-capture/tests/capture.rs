//! Capture contract tests over an injectable device.
//!
//! `Fake` stands in for cpal: it owns the device list and the bytes a "device"
//! produced, and lets the test decide when [`Capture::pump`] sees them. Every
//! test drives the same budget, conversion and drain logic the real source
//! uses; only the hardware is replaced.

use std::collections::VecDeque;
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::Duration;

use deskwaifu_audio_capture::{
    Capture, CaptureFactory, CaptureFormat, Clock, DeviceInfo, Error, ErrorCode, Event, Opened,
    SampleFormat, Source, MAX_PCM_BYTES,
};

/// Device id the fake reports as the default input.
const DEFAULT_ID: &str = "default-mic";
/// Device id the fake reports as a second, non-default input.
const OTHER_ID: &str = "other-mic";

fn info(id: &str, name: &str, is_default: bool) -> DeviceInfo {
    DeviceInfo {
        id: id.to_string(),
        name: name.to_string(),
        is_default,
    }
}

#[derive(Default)]
struct State {
    devices: Vec<DeviceInfo>,
    /// Format the fake device reports; `None` fails the open.
    format: Option<CaptureFormat>,
    /// Whether a device may be opened at all.
    openable: bool,
    /// Format the last `open` was asked for.
    requested: Option<CaptureFormat>,
    /// Device argument of the last `open`, `None` for the default.
    opened: Option<Option<String>>,
    /// Bytes already produced by the device and not yet taken.
    captured: VecDeque<u8>,
    /// Bytes every `take` moves out of `captured`.
    block: usize,
    /// Error the fake source reports.
    failure: Option<Error>,
    opened_count: usize,
}

#[derive(Clone)]
struct Fake(Arc<Mutex<State>>);

impl Fake {
    fn new(format: CaptureFormat) -> Self {
        let fake = Self::default();
        {
            let mut state = fake.state();
            state.devices = vec![
                info(DEFAULT_ID, "Default Mic", true),
                info(OTHER_ID, "Other Mic", false),
            ];
            state.format = Some(format);
            state.openable = true;
            state.block = usize::MAX;
        }
        fake
    }

    /// A fake whose monolithic channels are already 16 kHz mono PCM16.
    fn mono16k() -> Self {
        Self::new(CaptureFormat::REQUESTED)
    }

    fn state(&self) -> MutexGuard<'_, State> {
        self.0.lock().unwrap_or_else(|error| error.into_inner())
    }

    /// Emulates the device producing `bytes`.
    fn produce(&self, bytes: &[u8]) {
        self.state().captured.extend(bytes);
    }

    /// Emulates the backend reporting a stream error.
    fn report_error(&self) {
        self.state().failure = Some(Error::DEVICE);
    }

    fn opened(&self) -> usize {
        self.state().opened_count
    }

    /// Sets how many bytes one `take` moves out of the device buffer.
    fn set_block(&self, block: usize) {
        self.state().block = block;
    }
}

impl Default for Fake {
    fn default() -> Self {
        Self(Arc::new(Mutex::new(State::default())))
    }
}

impl CaptureFactory for Fake {
    fn devices(&mut self) -> Vec<DeviceInfo> {
        self.state().devices.clone()
    }

    fn open(&mut self, device: Option<&str>, requested: CaptureFormat) -> Result<Opened, Error> {
        let mut state = self.state();
        state.requested = Some(requested);
        state.opened = Some(device.map(str::to_string));
        if !state.openable {
            return Err(Error::UNAVAILABLE);
        }
        if let Some(named) = device {
            if !state.devices.iter().any(|entry| entry.id == named) {
                return Err(Error::UNAVAILABLE);
            }
        } else if !state.devices.iter().any(|entry| entry.is_default) {
            // The C++ `Start("")` selected `defaultAudioInput()`; without one
            // it failed instead of picking an arbitrary device.
            return Err(Error::UNAVAILABLE);
        }
        let Some(format) = state.format else {
            return Err(Error::START);
        };
        state.opened_count += 1;
        Ok(Opened {
            format,
            source: Box::new(FakeSource(Arc::clone(&self.0))),
        })
    }
}

struct FakeSource(Arc<Mutex<State>>);

impl Source for FakeSource {
    fn format(&self) -> CaptureFormat {
        self.0
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .format
            .expect("configured format")
    }

    fn take(&mut self) -> Vec<u8> {
        let mut state = self.0.lock().unwrap_or_else(|error| error.into_inner());
        let block = state.block.min(state.captured.len());
        state.captured.drain(..block).collect()
    }

    fn error(&self) -> Option<Error> {
        self.0
            .lock()
            .unwrap_or_else(|error| error.into_inner())
            .failure
    }
}

/// Clock the tests move by hand.
#[derive(Clone)]
struct ManualClock(Arc<Mutex<Duration>>);

impl ManualClock {
    fn new() -> Self {
        Self(Arc::new(Mutex::new(Duration::ZERO)))
    }

    fn advance(&self, by: Duration) {
        *self.0.lock().unwrap_or_else(|error| error.into_inner()) += by;
    }
}

impl Clock for ManualClock {
    fn now(&self) -> Duration {
        *self.0.lock().unwrap_or_else(|error| error.into_inner())
    }
}

fn capture(fake: &Fake) -> Capture<Fake> {
    Capture::with_factory(fake.clone())
}

fn capture_with_clock(fake: &Fake, clock: &ManualClock) -> Capture<Fake> {
    Capture::with_clock(fake.clone(), Box::new(clock.clone()))
}

/// Interleaved stereo `f32` frames from a mono signal.
fn stereo_f32(mono: &[f32]) -> Vec<u8> {
    mono.iter()
        .flat_map(|&sample| {
            let bytes = [sample.to_ne_bytes(), sample.to_ne_bytes()];
            bytes.concat()
        })
        .collect()
}

fn sine(frames: usize, rate: u32) -> Vec<f32> {
    (0..frames)
        .map(|i| (std::f32::consts::TAU * 440.0 * i as f32 / rate as f32).sin() * 0.4)
        .collect()
}

#[test]
fn lists_devices_with_the_gui_schema() {
    let fake = Fake::mono16k();
    let devices = capture(&fake).list_devices();
    assert_eq!(devices.len(), 2);
    assert_eq!(devices[0], info(DEFAULT_ID, "Default Mic", true));
    assert_eq!(devices[1], info(OTHER_ID, "Other Mic", false));
}

#[test]
fn start_uses_the_named_device_and_requests_16k_mono_pcm16() {
    let fake = Fake::new(CaptureFormat {
        sample_rate: 48_000,
        channels: 2,
        sample_format: SampleFormat::F32,
    });
    let mut capture = capture(&fake);
    capture.start(1, OTHER_ID, 60).unwrap();
    let state = fake.state();
    assert_eq!(state.opened, Some(Some(OTHER_ID.to_string())));
    assert_eq!(state.requested, Some(CaptureFormat::REQUESTED));
    drop(state);
    assert_eq!(capture.session(), Some(1));
}

#[test]
fn empty_device_opens_the_default_input() {
    let fake = Fake::mono16k();
    let mut capture = capture(&fake);
    capture.start(2, "", 60).unwrap();
    assert_eq!(fake.state().opened, Some(None));
    assert_eq!(capture.session(), Some(2));
}

#[test]
fn missing_named_device_is_a_hard_failure_without_fallback() {
    let fake = Fake::mono16k();
    let mut capture = capture(&fake);
    let error = capture.start(3, "unplugged-mic", 60).unwrap_err();
    assert_eq!(error, Error::UNAVAILABLE);
    assert_eq!(error.code, ErrorCode::Unavailable);
    // Nothing else was opened: the default input was never touched.
    assert_eq!(fake.opened(), 0);
    assert!(!capture.is_active());
    assert!(capture.take_events().is_empty());
}

#[test]
fn default_device_is_required_when_it_is_missing() {
    let fake = Fake::mono16k();
    fake.state().devices.clear();
    let mut capture = capture(&fake);
    assert_eq!(capture.start(4, "", 60).unwrap_err(), Error::UNAVAILABLE);
    assert_eq!(fake.opened(), 0);
}

#[test]
fn start_twice_fails_while_the_first_session_runs() {
    let fake = Fake::mono16k();
    let mut capture = capture(&fake);
    capture.start(1, "", 60).unwrap();
    assert_eq!(capture.start(2, "", 60).unwrap_err(), Error::BUSY);
    assert_eq!(capture.session(), Some(1));
    assert_eq!(fake.opened(), 1);
}

#[test]
fn duration_must_be_within_one_to_300_seconds() {
    let fake = Fake::mono16k();
    let mut capture = capture(&fake);
    assert_eq!(
        capture.start(1, "", 0).unwrap_err(),
        Error::INVALID_DURATION
    );
    assert_eq!(
        capture.start(1, "", 301).unwrap_err(),
        Error::INVALID_DURATION
    );
    assert_eq!(fake.opened(), 0);
    capture.start(1, "", 1).unwrap();
    capture.cancel();
    capture.start(1, "", 300).unwrap();
}

#[test]
fn stop_before_start_is_stale_and_cancel_before_start_is_a_noop() {
    let fake = Fake::mono16k();
    let mut capture = capture(&fake);
    // The C++ `Cancel` on an idle object clears state and reports nothing.
    capture.cancel();
    assert!(capture.take_events().is_empty());
    // `Stop` for an id that is not current is the C++ `Stale audio capture
    // session` failure.
    assert_eq!(capture.stop(1).unwrap_err(), Error::STALE);
}

#[test]
fn stop_rejects_a_stale_session_and_keeps_the_recording() {
    let fake = Fake::mono16k();
    let mut capture = capture(&fake);
    capture.start(9, "", 60).unwrap();
    fake.produce(&[0x01, 0x00, 0x02, 0x00]);
    assert_eq!(capture.stop(8).unwrap_err(), Error::STALE);
    assert_eq!(capture.session(), Some(9));
    let pcm = capture.stop(9).unwrap();
    assert_eq!(pcm, [0x01, 0x00, 0x02, 0x00]);
    // The C++ `Stop` also cleared `_active`, so another stop is stale again.
    assert_eq!(capture.stop(9).unwrap_err(), Error::STALE);
}

#[test]
fn stop_drains_bytes_not_yet_seen_by_pump() {
    let fake = Fake::mono16k();
    let mut capture = capture(&fake);
    capture.start(5, "", 60).unwrap();
    fake.produce(&[0x01, 0x00]);
    capture.pump();
    // The tail arrives after the last pump; `Stop` still reads it.
    fake.produce(&[0x02, 0x00, 0x03, 0x00]);
    let pcm = capture.stop(5).unwrap();
    assert_eq!(pcm, [0x01, 0x00, 0x02, 0x00, 0x03, 0x00]);
}

#[test]
fn cancel_discards_the_recording() {
    let fake = Fake::mono16k();
    let mut capture = capture(&fake);
    capture.start(6, "", 60).unwrap();
    fake.produce(&[0x01, 0x00]);
    capture.pump();
    capture.cancel();
    assert!(!capture.is_active());
    assert_eq!(capture.session(), None);
    assert!(capture.take_events().is_empty());
    assert_eq!(capture.stop(6).unwrap_err(), Error::STALE);
}

#[test]
fn passthrough_keeps_a_16k_mono_pcm16_stream_byte_for_byte() {
    let fake = Fake::mono16k();
    let mut capture = capture(&fake);
    capture.start(7, "", 60).unwrap();
    let input = [0x00, 0x80, 0xff, 0x7f, 0x01, 0x00];
    fake.produce(&input[..3]);
    capture.pump();
    fake.produce(&input[3..]);
    assert_eq!(capture.stop(7).unwrap(), input);
}

#[test]
fn passthrough_rejects_a_partial_native_frame() {
    let fake = Fake::mono16k();
    let mut capture = capture(&fake);
    capture.start(8, "", 60).unwrap();
    fake.produce(&[0x01]);
    assert_eq!(capture.stop(8).unwrap_err(), Error::INCOMPLETE_FRAME);
}

#[test]
fn converts_48k_stereo_float_to_16k_mono_pcm16() {
    let rate = 48_000;
    let fake = Fake::new(CaptureFormat {
        sample_rate: rate,
        channels: 2,
        sample_format: SampleFormat::F32,
    });
    let mut capture = capture(&fake);
    capture.start(10, "", 60).unwrap();
    let input = stereo_f32(&sine(rate as usize, rate));
    fake.produce(&input);
    capture.pump();
    let pcm = capture.stop(10).unwrap();
    // One second in gives one second out; the resampler's filter tail may add a
    // frame, never drop the recorded signal.
    let samples = pcm.len() / 2;
    assert!(
        samples.abs_diff(16_000) <= 1,
        "expected ~16000 samples, got {samples}"
    );
    assert!(pcm.iter().any(|&byte| byte != 0));
}

#[test]
fn silence_stays_silence_after_conversion() {
    let fake = Fake::new(CaptureFormat {
        sample_rate: 48_000,
        channels: 2,
        sample_format: SampleFormat::F32,
    });
    let mut capture = capture(&fake);
    capture.start(11, "", 60).unwrap();
    fake.produce(&vec![0u8; 48_000 * 2 * 4]);
    let pcm = capture.stop(11).unwrap();
    assert!(
        pcm.iter().all(|&byte| byte == 0),
        "silence must not pick up a signal"
    );
    assert!(pcm.len() / 2 <= 16_001);
}

#[test]
fn chunked_conversion_matches_one_block() {
    let rate = 48_000;
    let input = stereo_f32(&sine(rate as usize, rate));
    let run = |block: usize| {
        let fake = Fake::new(CaptureFormat {
            sample_rate: rate,
            channels: 2,
            sample_format: SampleFormat::F32,
        });
        fake.set_block(block);
        let mut capture = capture(&fake);
        capture.start(12, "", 60).unwrap();
        fake.produce(&input);
        while capture.input_bytes() < input.len() as u64 {
            capture.pump();
        }
        capture.stop(12).unwrap()
    };
    let whole = run(usize::MAX);
    assert_eq!(run(137), whole);
}

#[test]
fn budget_stops_appending_at_the_native_frame_boundary() {
    let fake = Fake::new(CaptureFormat {
        sample_rate: 16_000,
        channels: 2,
        sample_format: SampleFormat::I16,
    });
    let mut capture = capture(&fake);
    capture.start(13, "", 1).unwrap();
    // One second of stereo frames is 64 000 bytes; add a frame and a half more.
    let mut input = Vec::new();
    let mut expected = Vec::new();
    for i in 0..16_000i16 {
        let bytes = (i % 30_000 - 15_000).to_le_bytes();
        input.extend_from_slice(&bytes);
        input.extend_from_slice(&bytes);
        expected.extend_from_slice(&bytes);
    }
    input.extend_from_slice(&[0x7f; 6]);
    fake.produce(&input);
    capture.pump();
    assert!(capture.budget_reached());
    assert_eq!(capture.input_bytes(), 16_000 * 4);
    // A further pump and the normal stop still succeed with the exact budget.
    capture.pump();
    let pcm = capture.stop(13).unwrap();
    assert_eq!(pcm.len(), 16_000 * 2);
    assert_eq!(pcm, expected);
    assert!(capture.take_events().is_empty());
}

#[test]
fn budget_reached_still_has_the_independent_watchdog() {
    let fake = Fake::mono16k();
    let clock = ManualClock::new();
    let mut capture = capture_with_clock(&fake, &clock);
    capture.start(14, "", 1).unwrap();
    fake.produce(&[0u8; 32_002]);
    capture.pump();
    assert!(capture.budget_reached());
    // If the owner never stops, the independent watchdog still terminates it.
    clock.advance(Duration::from_millis(2_001));
    capture.pump();
    assert_eq!(capture.take_events(), vec![Event::Failed(14, Error::LIMIT)]);
    assert!(!capture.is_active());
}

#[test]
fn duration_watchdog_reports_the_limit_once_and_discards_the_audio() {
    let fake = Fake::mono16k();
    let clock = ManualClock::new();
    let mut capture = capture_with_clock(&fake, &clock);
    capture.start(15, "", 2).unwrap();
    fake.produce(&[0x01, 0x00]);
    // The C++ guard allowed one scheduling grace second on top of the limit.
    clock.advance(Duration::from_millis(2_900));
    capture.pump();
    assert!(capture.is_active());
    clock.advance(Duration::from_millis(101));
    capture.pump();
    assert_eq!(capture.take_events(), vec![Event::Failed(15, Error::LIMIT)]);
    assert!(!capture.is_active());
    assert_eq!(capture.stop(15).unwrap_err(), Error::STALE);
}

#[test]
fn stalled_device_cannot_grow_the_recording_past_the_duration_budget() {
    let fake = Fake::mono16k();
    let mut capture = capture(&fake);
    capture.start(16, "", 1).unwrap();
    // A device that keeps producing far more than one second is truncated at
    // the native budget, not accumulated.
    for _ in 0..8 {
        fake.produce(&vec![0x01u8; 64 * 1024]);
        capture.pump();
    }
    assert!(capture.budget_reached());
    assert_eq!(capture.input_bytes(), 32_000);
    assert!(capture.take_events().is_empty());
    let pcm = capture.stop(16).unwrap();
    assert_eq!(pcm.len(), 32_000);
}

#[test]
fn stuck_device_cannot_grow_the_recording_past_the_byte_cap() {
    // 8 kHz mono for the full 300 s: the native budget (4.8 MB) permits 9.6 MB
    // of 16 kHz output, exactly `MAX_PCM_BYTES`. A device that keeps producing
    // must stop at the cap instead of growing without limit.
    let fake = Fake::new(CaptureFormat {
        sample_rate: 8_000,
        channels: 1,
        sample_format: SampleFormat::I16,
    });
    let mut capture = capture(&fake);
    capture.start(17, "", 300).unwrap();
    for _ in 0..80 {
        fake.produce(&vec![0x00; 64 * 1024]);
        capture.pump();
    }
    match capture.stop(17) {
        Ok(pcm) => assert!(pcm.len() <= MAX_PCM_BYTES, "{} bytes", pcm.len()),
        // Both the assembled buffer and the resampler's own 300 s ceiling
        // report the same `audio_limit` classification.
        Err(error) => assert_eq!(error.code, ErrorCode::Limit),
    }
}

#[test]
fn device_error_is_reported_once_and_leaves_the_capture_stopped() {
    let fake = Fake::mono16k();
    let mut capture = capture(&fake);
    capture.start(18, "", 60).unwrap();
    fake.produce(&[0x01, 0x00]);
    fake.report_error();
    capture.pump();
    assert_eq!(
        capture.take_events(),
        vec![Event::Failed(18, Error::DEVICE)]
    );
    assert!(!capture.is_active());
    // Further pumps emit nothing: the failure was reported exactly once.
    capture.pump();
    assert!(capture.take_events().is_empty());
    // A partial recording is never handed out.
    assert_eq!(capture.stop(18).unwrap_err(), Error::STALE);
}

#[test]
fn stop_reports_a_device_error_instead_of_returning_audio() {
    let fake = Fake::mono16k();
    let mut capture = capture(&fake);
    capture.start(19, "", 60).unwrap();
    fake.produce(&[0x01, 0x00]);
    capture.pump();
    fake.report_error();
    assert_eq!(capture.stop(19).unwrap_err(), Error::DEVICE);
    assert!(!capture.is_active());
}

#[test]
fn unsupported_device_format_fails_to_start() {
    let fake = Fake::new(CaptureFormat {
        sample_rate: 0,
        channels: 1,
        sample_format: SampleFormat::I16,
    });
    let mut capture = capture(&fake);
    assert_eq!(
        capture.start(20, "", 60).unwrap_err(),
        Error::UNSUPPORTED_FORMAT
    );
    assert!(!capture.is_active());
    // The device was opened and then rejected, so it must be closed again.
    assert_eq!(fake.opened(), 1);
}

#[test]
fn a_failed_start_leaves_the_next_start_possible() {
    let fake = Fake::mono16k();
    fake.state().openable = false;
    let mut capture = capture(&fake);
    assert_eq!(capture.start(21, "", 60).unwrap_err(), Error::UNAVAILABLE);
    fake.state().openable = true;
    capture.start(21, "", 60).unwrap();
    assert!(capture.is_active());
}

/// Opt-in real-device smoke test.
///
/// With `DESKWAIFU_AUDIO_CAPTURE_TEST_DEVICE` unset this prints a skip line and
/// passes without touching hardware; set it to `1` to record ~0.2 s from the
/// default input.
#[test]
fn real_device_records_audio_when_enabled() {
    if std::env::var("DESKWAIFU_AUDIO_CAPTURE_TEST_DEVICE").as_deref() != Ok("1") {
        println!(
            "SKIP real_device_records_audio_when_enabled: set \
             DESKWAIFU_AUDIO_CAPTURE_TEST_DEVICE=1 to exercise real hardware"
        );
        return;
    }
    let mut capture = Capture::new();
    let devices = capture.list_devices();
    for device in &devices {
        println!(
            "device id={} name={} default={}",
            device.id, device.name, device.is_default
        );
    }
    capture.start(1, "", 1).expect("open the default input");
    let deadline = std::time::Instant::now() + Duration::from_millis(200);
    while std::time::Instant::now() < deadline {
        capture.pump();
        std::thread::sleep(Duration::from_millis(10));
    }
    let pcm = capture.stop(1).expect("stop the default input");
    println!("recorded {} samples", pcm.len() / 2);
    assert!(!pcm.is_empty(), "a real device must deliver audio");
}
