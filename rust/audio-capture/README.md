# Rust audio capture

Microphone capture for voice input, replacing the Qt Multimedia source in
`plugins/voice_input/private/audio/AudioCapture.cpp` and its `PcmNormalizer` on
the Rust side. Package `deskwaifu-audio-capture`, member of the root Cargo
workspace. Neither `rust/app` nor the `voice_input` plugin uses it yet.

## Scope

Only the capture engine:

* device enumeration in the shape the GUI reads (`id`, `name`, `is_default`);
* stream setup on a named input or the current default input;
* conversion of whatever the device delivers into 16 kHz mono PCM16
  little-endian;
* the recording bounds (1–300 s, a native-frame byte budget, a bounded callback
  buffer, an independent duration watchdog) and the `stop` / `cancel` outcomes;
* the failure classification the C++ `Failed` signal carried.

Deliberately out of scope: Qt, the plugin lifecycle, the controller state
machine, ASR, WAV encoding, clipboard access, settings persistence and any
retry or fallback behaviour.

There is no timer in this crate. The owner calls `Capture::pump` on its own
cadence (the Qt controller used 10 ms `readyRead` callbacks plus a 1 s status
timer) and collects failures with `Capture::take_events`.

## Native dependency

`cpal = "=0.18.1"` over its default host, ALSA on Linux. The cpal `pipewire`
feature is **not** enabled, for the same reason as in `rust/audio-output`:
`libspa-sys` runs bindgen and therefore needs libclang, which the packaged build
image does not ship. The ALSA default host reaches PipeWire through the
`pipewire-alsa` plugin, so a PipeWire desktop still records through PipeWire.

The resampler comes from `deskwaifu-audio-domain`
(`deskwaifu-audio-domain = { path = "../audio-domain" }`), which links system
`libsamplerate`. That is the same `SRC_SINC_FASTEST` converter the C++
`PcmNormalizer` used, so no second resampler exists in the tree.

Building the crate therefore needs `libasound2-dev` and `libsamplerate0-dev`
(or the distribution equivalents).

## Contract

| C++ `AudioCapture` | this crate |
| --- | --- |
| `ListDevices()` | `Capture::list_devices() -> Vec<DeviceInfo>` |
| `Start(session, device, maxSeconds, error)` | `Capture::start(session, device, max_seconds) -> Result<(), Error>` |
| `readyRead` → `OnAudioReady` | `Capture::pump()` |
| `Stop(session, pcm, error)` | `Capture::stop(session) -> Result<Vec<u8>, Error>` |
| `Cancel()` | `Capture::cancel()`, a no-op while idle |
| `Failed(session, code, message)` | `Event::Failed(session, Error)` via `take_events()` |
| `_budgetReached` | `Capture::budget_reached()` |

### 16 kHz mono PCM16

`start` requests 16 kHz, one channel, signed 16-bit little-endian. If the device
advertises that exact format it is used as is, and the captured bytes are copied
through untouched, the C++ `Append` fast path. Otherwise the device's own
default input configuration is taken — its rate, channel count and sample
encoding are kept — and every block is decoded to `f32`, averaged to mono and
resampled to 16 kHz by `deskwaifu-audio-domain`'s `Resampler`.

The result is always 16 kHz mono PCM16 little-endian, an even number of bytes.
A recording that ends in a partial native frame is rejected with
`Incomplete audio sample frame` instead of returning misaligned audio.

### Device selection

An empty device id selects the current default input at the moment `start` is
called. A named id is a token from `list_devices`; if it is not in the list any
more the call fails with `Audio input device is unavailable` and **nothing else
is opened**. There is no silent fallback to another microphone, matching
`docs/语音输入.md`.

### Bounds

* `max_seconds` outside 1–300 fails with `Invalid recording duration`.
* The native budget is `max_seconds × sample_rate × frame_bytes`, a whole
  number of frames. Reaching it stops appending, sets `budget_reached()`, and
  keeps draining and discarding the excess so scheduling jitter is not a
  failure — the owner then stops normally, as the Qt controller's timer did.
* If the owner never stops, `pump` fails with `audio_limit` after
  `max_seconds + 1` seconds of grace, the same independent watchdog the C++
  object kept.
* The converted PCM is capped at `MAX_PCM_BYTES` (300 s at 16 kHz mono), which
  fails with `Audio buffer limit exceeded`.
* The callback may buffer at most 4 MiB between drains; beyond that the oldest
  audio is dropped, so a stuck device cannot grow memory without limit.

### Failures

`start` and `stop` return the error; a failure discovered while recording
(libsamplerate error, oversized block, duration watchdog, device error) stops
the capture and queues exactly one `Event::Failed`. A device or conversion
error never panics and never returns partial audio from `stop`.

`cpal` reports `Xrun` (buffer overrun) through the same error callback as a real
device failure. An overrun is ignored: ALSA has already recovered the stream and
Qt's `QAudioSource` kept running through it. Every other error is fatal.

## Not wired yet

* `rust/app` and the `voice_input` plugin still use `AudioCapture.cpp`; nothing
  in C++ or CMake calls into this crate.
* The plugin's controller state machine, session ids, max-duration snapshot and
  `capture_failed` reply mapping are not ported.
* Packaging does not list the new crate, and the CI image dependency set has not
  been extended for it.
* Permissions (microphone access on a sandboxed desktop), the PipeWire graph
  policy and the voice-input settings page are untouched.

## Tests

```
cargo test                        # no hardware, no microphone
cargo clippy --all-targets -- -D warnings
```

The tests inject a fake `CaptureFactory`, so the state machine, budget,
conversion and failure paths run without a sound card. Device enumeration,
start/stop/cancel ordering, the no-fallback rule, 48 kHz stereo float → 16 kHz
mono conversion, chunk invariance, silence, limits and the once-only device
failure all have coverage.

A real microphone is only touched opt-in:

```
DESKWAIFU_AUDIO_CAPTURE_TEST_DEVICE=1 cargo test real_device -- --nocapture
```

That records about 0.2 s from the default input, prints the device list and
sample count, and asserts a nonzero length. Without the variable the test prints
a skip line and claims nothing.

## Known differences from the C++ implementation

* Failures are synchronous (`pump`/`stop` return an `Error` and queue one
  event) instead of a queued Qt signal, so the C++ generation counter that
  protected against a stale queued failure has no equivalent. The C++ window in
  which `Stop` still returned the pending failure message therefore collapses:
  once `pump` has reported `Event::Failed`, `stop` answers with
  `Stale audio capture session`. Neither path hands out partial audio.
* The C++ `PcmNormalizer` accepted `UInt8`/`Int16`/`Int32`/`Float`. This crate
  also decodes signed 8-bit, unsigned 16-bit, unsigned 32-bit and `f64`, because
  cpal can deliver them; `I24`/`U24`/`I64`/`U64`/DSD are refused as an
  unsupported format.
* The device list comes from cpal's ALSA enumeration, so it also contains ALSA
  plugins (`null`, `pulse`, `jack`, …) that Qt did not list. `null` records
  silence.
* Device ids are the base64url encoding of the cpal device id
  (`alsa:hw:2,0`), not of a Qt `QAudioDevice::id()` byte array. They are stable
  across runs but are not interchangeable with ids saved by the C++ plugin.
* A stream cpal cannot open at the requested format falls back to the device
  default instead of failing: Qt required the requested format, cpal's ALSA
  `plughw` layer already converts, and the crate resamples what arrives.
* `cancel()` before `start()` is a silent no-op, exactly like the C++
  `Cancel()`; `stop()` before `start()` or with a stale id fails with
  `Stale audio capture session`.
