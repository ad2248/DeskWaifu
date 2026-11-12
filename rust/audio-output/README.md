# Rust audio output

PCM playback engine for synthesized speech, replacing the Qt Multimedia player
in `plugins/speech/private/processor/speech/audio/AudioPlayer.cpp` on the Rust
side. Member of the root Cargo workspace (`deskwaifu-audio-output`); the speech
plugin and `deskwaifu-rust` do not use it yet.

## Scope

Only the playback engine: a bounded pending buffer, conversion of the source
PCM16 to the device format, and the `PlaybackStarted` / `Drained` / `Failed`
outcomes the C++ player emits as Qt signals.

Deliberately out of scope: the network TTS protocol, the speech plugin, the
`SpeechPlayer` state machine, credentials, the Qt event loop and any plugin
lifecycle. There is no timer in this crate; the owner calls `Player::pump`,
normally at the C++ player's 10 ms cadence.

## Contract

| C++ `AudioPlayer` | this crate |
| --- | --- |
| `Start(id, format)` | `Player::start(id, SourceFormat)`: discards the previous playback, opens the default output device |
| `Append(id, pcm)` | `Player::append(id, &[u8])`: current id only, ignored after `FinishInput`, 8 MiB limit |
| `FinishInput(id)` | `Player::finish_input(id)`: rejects a trailing partial frame |
| `Stop(id)` | `Player::stop(id)`: drops queue and stream, never emits `Drained` |
| `PlaybackStarted` | `Event::PlaybackStarted(id)` once the device pulls a buffer |
| `Drained` | `Event::Drained(id)`, only after end of input, empty queue and device idle |
| `Failed` | `Event::Failed(id, Error)` with `ErrorCode::AudioDevice`, `BufferOverflow` or `Protocol` |
| 10 ms `QTimer` `Pump` | `Player::pump()` |

Outcomes are queued, and `Player::take_events()` drains them in emission order.

## How "drained" is guaranteed

The C++ player reports `Drained` when `_finished` holds, `_pending` is empty and
the sink is in `QAudio::IdleState`, i.e. Qt has handed every written byte to the
device and its buffer is empty. The two conditions that matter are kept:

1. every queued source frame is converted and written into a device callback
   buffer (`Queue::fill` under the queue lock);
2. the device consumed the buffers it was given, tracked by a "played"
   watermark that the callback advances to the previously delivered frame count
   before it fills the next buffer.

`Player::pump` reports `Drained` only when the queue is empty, `finish_input`
already happened and the watermark covers everything delivered. That costs at
most one callback period of latency after the final buffer, which is exactly the
moment Qt reached its idle state, and it never reports a drain while audio is
still sitting in the device buffer.

## Buffering and threading

- One `Arc<Mutex<Queue>>` per playback holds unmodified source PCM16. The
  player and the audio callback lock it only for one `copy_from_slice` or one
  callback buffer; no device call, allocation of consequence or logging happens
  under the lock.
- The pending limit is exactly the C++ `8 * 1024 * 1024` **source** bytes.
  Exceeding it fails with `Error::OVERFLOW` (`BufferOverflow`) and stops the
  playback; exactly 8 MiB is accepted, one byte more is not.
- `play`, `append` and `finish_input` are ignored for any id that is not
  current, and ids are single-use.

## Format conversion

Sources always produce signed 16-bit interleaved little-endian PCM
(`QAudioFormat::Int16`, 24 kHz mono from MiniMax). The device keeps its own
channel count and sample format; samples are normalized to `f32` in `[-1, 1]`
and encoded as `I8`, `U8`, `I16`, `U16`, `I32`, `U32`, `F32` or `F64`.

- Mono source into N device channels duplicates the sample into every channel.
- Multi-channel source into mono averages the channels in `f64`, matching
  `deskwaifu_audio_domain::normalize_pcm`.
- Multi-channel into multi-channel maps device channel `c` to source channel
  `min(c, sources - 1)`, so a narrower source is duplicated and a wider source
  keeps the first device-channel-count channels.
- The **sample rate is never converted by this crate**. The stream asks the
  server for the source rate; PipeWire's graph (or ALSA's plug layer) either
  accepts it and resamples downstream, or refuses to open the stream, which is
  reported as `AudioDevice` like Qt's `isFormatSupported` rejection.

Encoding reuses the scaling the rest of the repo uses for PCM16
(`s / 32768`, round to nearest, clamp); non-finite intermediate values encode as
silence. Unlike the C++ player, which never converts anything because Qt
refuses a format the device does not support, this crate negotiates channel
count and sample format with the device. That difference is intentional: cpal
needs an explicit sample type, and speaking 24 kHz mono through a PipeWire
server would otherwise be impossible.

## Dependency

`cpal 0.18.1` with its default host set (ALSA on Linux, including the
`pipewire-alsa` PCM plugin that routes into PipeWire). The default host is tried
first and any other available host only when it has no default output; the host
is chosen through `cpal::host_from_id`, not `cpal::default_host`, because the
latter can panic.

cpal's `pipewire` feature is deliberately **not** enabled: it pulls
`libspa-sys`, whose build script needs `libclang` for bindgen, which the packaged
build image does not ship. Playing through ALSA's `pipewire-alsa` plugin reaches
the same PipeWire graph without a build-time clang dependency.

PulseAudio compatibility on this platform is provided by `pipewire-pulse` (plus
`wireplumber` as the session manager), which publishes the PulseAudio protocol
on the same socket. This crate does not link `libpulse` and does not enable
cpal's `pulseaudio` feature: there is no separate PulseAudio client path to
maintain, and PulseAudio-only systems without PipeWire are not a target.

## Testing

```sh
cargo fmt --manifest-path rust/audio-output/Cargo.toml -- --check
cargo test --offline --manifest-path rust/audio-output/Cargo.toml
cargo clippy --offline --manifest-path rust/audio-output/Cargo.toml --all-targets -- -D warnings
```

- `tests/playback.rs` drives the real queue, converter and drain logic through
  an injected sink (`SinkFactory`); no hardware is touched. It covers stale ids,
  append/stop/finish ordering, the exact overflow boundary, zero-length appends,
  format conversion and start/stop/start with a fresh id.
- `tests/device.rs` is opt-in: with `DESKWAIFU_AUDIO_TEST_DEVICE=1` it plays
  half a second of silence on the default device and asserts `Drained`.
  Without the variable it prints that hardware output is unverified and
  returns; it does not claim a passing hardware assertion, following
  `rust/cubism-core/tests/core.rs` and
  `rust/cubism-render/tests/surfaceless.rs`.

Skipping or passing the opt-in test says nothing about audio quality, latency,
suspend/resume or device hot-plug behaviour.

## Not wired yet

- The speech plugin still uses `AudioPlayer`; nothing in `plugins/` calls this
  crate, and the C++ `SpeechPlayer`/`SpeechProcessor` contract is unchanged.
- `deskwaifu-rust` does not depend on it, and the crate is not a member of
  `rust/Cargo.toml`.
- No CMake target, packaging rule or CI job includes it, and no AUR or release
  artifact is built from it.
- No resampling: a device that cannot run at the requested source rate fails
  instead of being converted. `deskwaifu-audio-domain`'s resampler targets
  16 kHz voice input and links system `libsamplerate`, so it is not reused for
  playback.
