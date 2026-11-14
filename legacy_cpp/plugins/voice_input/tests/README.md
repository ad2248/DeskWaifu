# Voice input integration smoke

`voice_input_smoke.py` is a **container-only, offline** integration test. Missing
runtime dependencies, unavailable audio, or missing test modules are failures,
not skipped coverage. Do not run it on the user's current desktop.

Build `voice-input-smoke-fixture` along with the normal desktop, IPC and application
targets, then run inside the pinned CI container:

```sh
python3 scripts/test-environment.py \
  python3 plugins/voice_input/tests/voice_input_smoke.py \
  build-ci "$DESKWAIFU_DEPS_DIR/CubismSdkForNative-5-r.5/Samples/Resources" \
  --logs build-ci/voice-input-smoke-logs
```

The mock's contract self-tests need no compositor, microphone, credentials or
container runtime (they bind only an ephemeral loopback port):

```sh
python3 -m unittest discover -s plugins/voice_input/tests/fixtures -p test_smoke_mock.py -v
```

They cover successful native requests, exact returned whitespace, intentional
503s, silent input, missing/nonzero 250 ms WAV padding, bad API paths/dummy keys,
wrong models, invalid WAVs, the session/credential environment whitelist, and
phase-independent 1 kHz marker detection versus other loud audio. Cleanup tests
mock process-group signaling: even exited leaders and TERM-ignoring descendants
must receive the final SIGKILL pass; errors must not prevent other groups being
signaled, the mock server being closed, or the summary being saved. Signal-handler
tests cover SIGTERM and SIGINT taking the exception/finally cleanup path.

Required commands: `dbus-daemon`, `sway`, `swaymsg`, `pipewire`, `pipewire-pulse`,
`wireplumber` (0.5 configuration syntax), `pw-loopback`, `pw-play`, `wpctl`,
`wl-copy`, and `wl-paste`. No packages are installed by the test. The application
requires its normal shared libraries, Qt Wayland/multimedia runtime plugins and
software OpenGL support. Sway must be runnable by the container's unprivileged CI
user; do not pass through host audio/video devices, compositor sockets or user
configuration directories.

## Actual chain exercised

1. A fresh environment whitelist, HOME, XDG directories and D-Bus session are
   created. No host credentials, proxy configuration, display sockets or Pulse
   server settings are inherited. WirePlumber's hardware monitors are disabled.
2. Private PipeWire/Pulse/WirePlumber instances create `test-sink -> test-mic`
   using `pw-loopback`. `wpctl` verifies the default source, the desktop IPC device
   list must contain `test-mic`, and the selected device ID is saved explicitly.
   Startup polls retry transient `wpctl inspect` failures while WirePlumber is
   still publishing default-source metadata; dependencies still fail on timeout.
3. A private headless Sway compositor hosts the **production desktop plugin**.
   Only the real desktop/IPC modules and the test voice module are copied to the
   private plugin root. SDK Haru resources are copied into private models.
4. The fixture module compiles `VoiceInputPlugin.cpp` with a constructor-only
   substitution (`VoiceInputSmokePlugin.cpp`). It still uses real AudioCapture,
   QAudioSource, PCM normalization, controller, WAV encoding, NativeAsrClient,
   event bus and desktop ClipboardService. It changes only the ASR transport's
   host/scheme/port to a loopback HTTP server. Production endpoint validation
   accepts only HTTPS/WSS, and the smoke checks that configuring HTTP is rejected.
5. `pw-play` plays the checked-in speech PCM plus a 250 ms tone marker into the
   virtual sink. No physical microphone or fake capture class is used. The local
   mock validates native API path, dummy authorization, model, native protocol
   parameters, base64 data and mono/16-bit/16 kHz WAV. It rejects silent capture.
   It never performs cloud recognition: returned Chinese text is deterministic.
6. The real desktop ClipboardService invokes `wl-copy`, and an **independent
   `wl-paste --no-newline` process** retrieves UTF-8 bytes without stripping text.
   Chinese, embedded newline/tab, leading/trailing spaces and a final newline
   must survive byte-for-byte. The controller must report `copy_state=committed`.

## Assertions and artifacts

- Sway starts on a new empty workspace. Its tree must contain no focused client
  (only workspace/output/root focus is allowed), and no normal window. Desktop
  Wayland protocol logs must show `set_keyboard_interactivity(0)` and no
  `wl_keyboard.enter`. These checks run before recording and after copying.
- Every uploaded WAV must end with **exactly 4000 verified zero samples** (the
  encoder's 250 ms padding at 16 kHz). Independently, the first recording's last
  250 ms of **captured** PCM (WAV samples `[-8000:-4000]`, before padding) must
  contain the 1 kHz ending marker: peak must exceed 1000 and phase-independent
  1 kHz energy fraction must be at least 0.35 (not merely non-silent speech).
  Manual capture length must match the supplied speech plus marker within
  ±8000 samples (500 ms), allowing startup/playback buffering but rejecting gross
  truncation or a recording that runs long. `request-0.wav` retains the full
  audio; summaries report total/captured/padding sample counts, capture-tail
  peak and the marker score. Playback completes before Stop is sent, so pending
  Qt reads are drained through the real Stop path. This is not a sample-exact
  latency measurement or proof that every marker sample survives: playback
  drain and capture buffering/resampling can alter marker placement.
  Unit tests in `AudioTest.cpp` separately exercise precise stop/drain boundaries.
  Semantic ASR correctness is not claimed for the deterministic mock.
- The seeded clipboard is held by a registered, independently grouped
  `wl-copy --foreground` process reading a private prewritten file. Its output
  goes to a log file, not a captured pipe that a daemon could keep open. Readiness
  polls independent `wl-paste` bytes (retrying a missing initial selection), and
  the holder exiting before readiness is an immediate failure.
- Cancelling a recording must produce `cancelled`, make no new ASR request and
  preserve a seeded clipboard byte-for-byte.
- An intentional ASR HTTP 503 must report `asr_http` and preserve the clipboard.
- A two-second configured recording limit must trigger recognition and copying
  without any stop command; elapsed duration must remain in the bounded window.
  Actual captured PCM must contain 24000–32000 samples (1.5–2 s), never exceeding
  the configured 32000-sample budget; the entire WAV has at most 36000 samples
  including its separately checked 4000-sample zero padding.
- All service/desktop logs, `focus-tree.json`, uploaded WAVs and mock request
  summaries remain in `--logs`, including on failure. Temporary private runtime
  state is removed, and the desktop must exit cleanly after `quit` on success.
  SIGTERM/SIGINT raise an interruption exception so `finally` still runs; repeated
  termination signals are ignored during cleanup. All registered private process
  groups receive TERM followed by unconditional KILL, even if their leaders have
  already exited. Nested cleanup ensures server shutdown/close and summary save
  are attempted even if stopping processes or shutting down the server fails.

The dummy key is public test data, never loaded from any host environment or
file. Logs include Wayland protocol traffic and synthetic/supplied test audio;
they do not contain a real API key or microphone recording. This smoke does not
exercise live cloud ASR or a physical microphone and is not a GUI settings-panel
interaction test.
