//! Recording state machine.
//!
//! Port of `AudioCapture.cpp`: same guards, same budget, same failure codes.
//! The only structural difference is that Qt's `readyRead` callback becomes
//! [`Capture::pump`], which the owner calls on its own cadence, and that
//! failures are returned through [`Capture::take_events`] instead of a queued
//! `Failed` signal.

use std::time::Duration;

use crate::device::CpalFactory;
use crate::normalize::Normalizer;
use crate::{CaptureFactory, CaptureFormat, DeviceInfo, Error, ErrorCode, Event, Source};

/// Longest recording the C++ capture accepted.
pub const MAX_SECONDS: u32 = 300;

/// Extra second the C++ watchdog allowed beyond the recording limit.
const WATCHDOG_GRACE: u64 = 1;

/// Bytes per read the C++ drain loop used; also the block size at which it
/// re-checked the elapsed time.
const READ_BLOCK: usize = 65_536;

/// Monotonic time source for the duration watchdog.
///
/// The seam exists so tests can drive the limit without sleeping.
pub trait Clock: Send {
    /// Reading that increases monotonically, in the clock's own epoch.
    fn now(&self) -> Duration;
}

/// Wall-clock monotonic source used by [`Capture::new`].
#[derive(Debug)]
pub struct SystemClock(std::time::Instant);

impl SystemClock {
    /// Starts the clock at the current instant.
    pub fn new() -> Self {
        Self(std::time::Instant::now())
    }
}

impl Default for SystemClock {
    fn default() -> Self {
        Self::new()
    }
}

impl Clock for SystemClock {
    fn now(&self) -> Duration {
        self.0.elapsed()
    }
}

/// Microphone capture for one session at a time.
pub struct Capture<F: CaptureFactory = CpalFactory> {
    factory: F,
    clock: Box<dyn Clock>,
    session: Option<u64>,
    source: Option<Box<dyn Source>>,
    normalizer: Option<Normalizer>,
    max_seconds: u32,
    max_input_bytes: u64,
    input_bytes: u64,
    budget_reached: bool,
    started_at: Option<Duration>,
    events: Vec<Event>,
}

impl Default for Capture<CpalFactory> {
    fn default() -> Self {
        Self::new()
    }
}

impl Capture<CpalFactory> {
    /// Creates a capture that opens the default host's input devices.
    pub fn new() -> Self {
        Self::with_factory(CpalFactory)
    }
}

impl std::fmt::Debug for Capture<CpalFactory> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Capture")
            .field("session", &self.session)
            .field("budget_reached", &self.budget_reached)
            .finish()
    }
}

impl<F: CaptureFactory> Capture<F> {
    /// Creates a capture over `factory`; tests inject a fake device here.
    pub fn with_factory(factory: F) -> Self {
        Self::with_clock(factory, Box::new(SystemClock::default()))
    }
}

impl<F: CaptureFactory> Capture<F> {
    /// Creates a capture over `factory` with an explicit duration clock.
    pub fn with_clock(factory: F, clock: Box<dyn Clock>) -> Self {
        Self {
            factory,
            clock,
            session: None,
            source: None,
            normalizer: None,
            max_seconds: 0,
            max_input_bytes: 0,
            input_bytes: 0,
            budget_reached: false,
            started_at: None,
            events: Vec::new(),
        }
    }

    /// Input devices as the GUI expects them: `id`, `name`, `is_default`.
    pub fn list_devices(&mut self) -> Vec<DeviceInfo> {
        self.factory.devices()
    }

    /// Current session id, `None` while idle.
    pub const fn session(&self) -> Option<u64> {
        self.session
    }

    /// Whether audio is being captured.
    pub const fn is_active(&self) -> bool {
        self.session.is_some()
    }

    /// Whether the native input budget was consumed.
    ///
    /// Mirrors the C++ `_budgetReached`: further input keeps being drained and
    /// discarded, and the owner stops normally.
    pub const fn budget_reached(&self) -> bool {
        self.budget_reached
    }

    /// Native bytes accepted so far.
    pub const fn input_bytes(&self) -> u64 {
        self.input_bytes
    }

    /// Removes and returns the events emitted since the last call.
    pub fn take_events(&mut self) -> Vec<Event> {
        std::mem::take(&mut self.events)
    }

    /// Starts capturing for `session` from `device`, or the default input when
    /// `device` is empty.
    ///
    /// Fails with [`Error::BUSY`] while another session runs and with
    /// [`Error::INVALID_DURATION`] outside 1–[`MAX_SECONDS`] seconds, in the
    /// C++ check order. `device` is an id from [`Self::list_devices`]; when it
    /// is not available the call fails with [`Error::UNAVAILABLE`] and nothing
    /// else is opened, so a saved device never silently becomes the default
    /// microphone.
    pub fn start(&mut self, session: u64, device: &str, max_seconds: u32) -> Result<(), Error> {
        if self.is_active() {
            return Err(Error::BUSY);
        }
        if !(1..=MAX_SECONDS).contains(&max_seconds) {
            return Err(Error::INVALID_DURATION);
        }
        self.cancel();
        let named = (!device.is_empty()).then_some(device);
        let opened = self.factory.open(named, CaptureFormat::REQUESTED)?;
        let normalizer = Normalizer::new(opened.format)?;
        let frame_bytes = opened.format.frame_bytes() as u64;
        self.max_seconds = max_seconds;
        // The budget is a whole number of native frames, like the C++
        // `maxSeconds * sampleRate * bytesPerFrame`.
        self.max_input_bytes =
            u64::from(max_seconds) * u64::from(opened.format.sample_rate) * frame_bytes;
        self.input_bytes = 0;
        self.budget_reached = false;
        self.started_at = Some(self.clock.now());
        self.normalizer = Some(normalizer);
        self.source = Some(opened.source);
        self.session = Some(session);
        Ok(())
    }

    /// Drains the device, then stops and returns the 16 kHz mono PCM16.
    ///
    /// Fails with [`Error::STALE`] for an id that is not current, and with the
    /// drain or conversion error without returning any audio, so a partial
    /// recording is never handed out.
    pub fn stop(&mut self, session: u64) -> Result<Vec<u8>, Error> {
        if self.session != Some(session) {
            return Err(Error::STALE);
        }
        if let Err(error) = self.drain() {
            self.reset();
            return Err(error);
        }
        let normalizer = self.normalizer.take();
        self.reset();
        let Some(mut normalizer) = normalizer else {
            return Err(Error::STALE);
        };
        normalizer.finish()
    }

    /// Discards the current recording and closes the device.
    ///
    /// A no-op while idle, like the C++ `Cancel`, which every teardown path
    /// goes through. Nothing is reported.
    pub fn cancel(&mut self) {
        self.reset();
    }

    /// Drains what the device captured since the last call.
    ///
    /// Reaching the duration budget truncates further input and sets
    /// [`Self::budget_reached`]; the owner stops normally, exactly like the C++
    /// controller's timer. A device or conversion failure stops the capture and
    /// queues [`Event::Failed`] exactly once. Call it periodically: this is
    /// also where the duration watchdog is evaluated.
    pub fn pump(&mut self) {
        if !self.is_active() {
            return;
        }
        if self.over_time() {
            self.fail(Error::LIMIT);
            return;
        }
        match self.drain() {
            Ok(()) => {}
            Err(error) => self.fail(drain_code(error)),
        }
    }

    /// Reads everything the device buffered, bounded by the duration budget.
    fn drain(&mut self) -> Result<(), Error> {
        if self.over_time() {
            return Err(Error::LIMIT);
        }
        let Some(source) = self.source.as_mut() else {
            return Ok(());
        };
        if let Some(error) = source.error() {
            return Err(error);
        }
        // Drain only the bytes available at this boundary, not an
        // indefinitely replenished stream.
        let available = source.take();
        let mut offset = 0;
        while offset < available.len() {
            if self.over_time() {
                return Err(Error::LIMIT);
            }
            let block = READ_BLOCK.min(available.len() - offset);
            let bytes = &available[offset..offset + block];
            // Excess samples are scheduling jitter, not a capture failure: the
            // remainder is drained and discarded, like the C++ loop.
            let room = self.max_input_bytes.saturating_sub(self.input_bytes);
            let accepted = usize::try_from(room).map_or(bytes.len(), |room| room.min(bytes.len()));
            if accepted > 0 {
                let normalizer = self.normalizer.as_mut().ok_or(Error::INACTIVE)?;
                normalizer.append(&bytes[..accepted])?;
                self.input_bytes += accepted as u64;
                if self.input_bytes == self.max_input_bytes {
                    self.budget_reached = true;
                }
            }
            offset += block;
        }
        Ok(())
    }

    /// Whether the C++ `maxSeconds + 1` grace second has passed.
    fn over_time(&self) -> bool {
        let Some(started) = self.started_at else {
            return false;
        };
        let limit = Duration::from_secs(u64::from(self.max_seconds) + WATCHDOG_GRACE);
        self.clock.now().saturating_sub(started) > limit
    }

    /// Stops the capture and reports `error` once, as the C++ `Fail` did.
    fn fail(&mut self, error: Error) {
        let session = self.session;
        self.reset();
        if let Some(session) = session {
            self.events.push(Event::Failed(session, error));
        }
    }

    /// Closes the device and drops every buffer, keeping queued events.
    fn reset(&mut self) {
        self.source = None;
        if let Some(mut normalizer) = self.normalizer.take() {
            normalizer.clear();
        }
        self.session = None;
        self.max_seconds = 0;
        self.max_input_bytes = 0;
        self.input_bytes = 0;
        self.budget_reached = false;
        self.started_at = None;
    }
}

/// Rewrites a drain failure the way the C++ `OnAudioReady` did: every `Drain`
/// error was reported with the `audio_limit` code. A device stream error is the
/// exception, because it arrived through `stateChanged` as `audio_device`
/// instead.
fn drain_code(error: Error) -> Error {
    match error.code {
        ErrorCode::Limit | ErrorCode::Device => error,
        _ => Error {
            code: ErrorCode::Limit,
            message: error.message,
        },
    }
}
