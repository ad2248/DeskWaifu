# deskwaifu-rust

The Rust model runtime behind `deskwaifu-rust`. It owns the model catalog, the
Cubism Core model, the IPC socket, model metadata, hotkeys and external control
sources.

## Two modes

By default the runtime is **headless**: it loads the current model into Cubism
Core, answers IPC requests, and runs the same 16 ms update tick as the desktop
runtime. Nothing is drawn and no compositor is needed, which is what the CI and
most tests exercise.

### `--overlay` (opt-in rendering)

```sh
CUBISM_SDK_ROOT=/path/to/CubismSdkForNative-5-r.5 \
  deskwaifu-rust --overlay --model /path/to/Haru.model3.json --socket /tmp/model.sock
```

`--render` is accepted as an alias. With the flag the runtime additionally:

- connects to `WAYLAND_DISPLAY` and creates a `zwlr_layer_shell_v1` overlay
  (640x480, overlay layer, top-left anchor, no keyboard focus)
- starts the Cubism Framework with the SDK's external
  `Framework/src/Rendering/OpenGL/Shaders/Standard` directory and uploads the
  model's textures to GL
- draws one canvas-fitting frame per 16 ms tick, honouring `zoom`, `x`, `y` and
  `hidden` from the IPC transform commands
- derives the Wayland **input region** from every drawn frame's alpha, so the
  overlay is click-through exactly where the model is transparent (see below)
- rebuilds the rendered model, textures and renderer on `set_model`,
  `next_model` and `prev_model`
- applies the C++ `InteractionProcessor` pointer policy to the events the
  overlay reports, so the character really reacts to the mouse (see below)
- plays the model's **first declared motion in a group named `Idle`**, if the
  model declares one, and blinks using the `Groups` entry named `EyeBlink`.
  A tap can additionally start the `TapBody` group and the declared
  expressions. No motion is invented when none is declared, and there is no
  random selection.

Failing to reach the compositor, start the Framework, read a texture or create
the renderer is a startup error: the process exits non-zero with the reason on
stderr instead of opening a blank window.

`--overlay` requires `WAYLAND_DISPLAY` and `CUBISM_SDK_ROOT`. Building the
runtime at all also requires `GLEW_ROOT` (CMake passes it; a plain
`cargo build`/`cargo test` in this directory needs it exported).

### Click-through

Only the drawn model receives pointer input. Every frame, after drawing and
before presenting, the runtime reads the drawn buffer back and derives the
surface-local input region from its alpha, then installs it with
`wl_surface.set_input_region`:

- `deskwaifu_model_domain::extract_input_regions` samples 10x10 blocks at their
  centre and treats alpha **above 10** as opaque, producing clipped
  top-left-origin runs — the same block size, threshold and clipping as the C++
  window's `ExtractModelInputRegion`.
- A fully transparent frame (for example while `toggle_hidden` hides the model)
  yields an empty region: the whole surface is click-through. A fully opaque
  frame yields regions covering the whole surface.
- Nothing is re-clipped or merged afterwards: `Overlay::set_input_region` only
  unions the rectangles into a `wl_region`.

The region is recomputed on **every** frame, which is what the C++ does: its
render thread reads the model framebuffer every 16 ms and its interaction thread
calls `SetInputRegion` on every presented frame, with no change detection. Since
the region follows the animated model, skipping frames would only save work on
frames that did not move. The C++ derives the region from the model framebuffer
*before* display opacity is applied, so hover dimming never changes
click-through; this runtime has no hover opacity, so the presented buffer is the
same buffer.

If `set_input_region` fails, the frame fails as a renderer error rather than
reporting a region that never reached the compositor.

### Pointer interaction

Every tick the runtime drains `deskwaifu_overlay::Overlay::take_events()` and
applies `InteractionProcessor`'s policy to the model the overlay is drawing.
The counters are reported in `get_status`/`debug_frame`, so a client can see that
the policy really ran instead of inferring it from pixels.

| C++ behaviour | this runtime |
| --- | --- |
| `IsTap`: both axis displacements **strictly** below 10 px | same threshold (`TAP_DISTANCE`) |
| `ModelDoubleClick::Delay = 350 ms`, `Distance = 10 px` | same interval and distance |
| a lone tap is dispatched by `interaction.tap_timeout` after `Delay` | the tick flushes it once that many milliseconds have really passed; the C++ imposes **no** dwell limit on press-to-release, so neither does this |
| a second click inside the window cancels the pending tap | same, and counted as `double_clicks` |
| a release beyond the threshold is a drag; `PointerMoved` applies `InputController::Drag` | same math, applied to the IPC-owned `View` (`x`/`y`) |
| `PointerScroll` zooms by `1 + delta * .1` around the cursor | same, through `Transform::zoom_around` (scale clamped to `0.1..=10`) |
| right **release** sends `next_model` | the tick runs the same `select` path as the IPC command |
| middle **press** sends `switch_skin` | counted as `middle_clicks`, nothing else: the runtime has no skins |
| `hidden && !screenEditing` returns early | a hidden model ignores every tap, drag, right/middle click and scroll; the recogniser is cleared, as `InteractionCancel` does |
| tap on `Head` starts `random_expression` | next declared expression (round-robin, which is what the C++ cursor does) |
| tap on `Body` starts `random_motion` `TapBody` and `random_expression` | next `TapBody` motion plus next declared expression |

**Hit testing.** The C++ `LAppModel::HitTest` looks up the declared `HitAreas`
entry by name, then tests the point against that drawable's axis-aligned vertex
bounds (`IsHit`), after the point has been mapped through the projection.
`LAppLive2DManager::HitSnapshot` builds that mapping from the same
`BuildModelProjection` the draw uses. This runtime maps the surface point with
`surface_to_model`, the exact inverse of the matrix `Scene::mvp` draws with
(canvas fit, then `zoom`, then `x`/`y`).

`cubism-render` exposes no drawable vertices, so the declared hit areas cannot be
placed. For a model that declares `HitAreas` the runtime therefore resolves **no**
part, sets `hit_test_available:false` and reports why in `interaction_error`; it
does not guess, and starts nothing.

A model that declares **no** `HitAreas` uses the C++
`LAppModel::HitTest` fallback, which needs only the projection: `Head` when the
model-space `y` is above the midline (`y > 0`), `Body` on or below it. That is
implemented for real, and proved live on a copy of the fixture with its
`HitAreas` section removed.

**Deliberate differences.**

- A **double-click** in the C++ opens the keyboard grab and emits
  `interaction.double_click/v1`. This runtime has no keyboard grab, no key
  binding surface and no notification bus, so a double-click only cancels the
  pending tap and is counted. It starts no speech greeting and no grab.
- A **middle click** does nothing: `switch_skin` has no counterpart because a
  model directory holds one skin here.
- A **drag** edits the in-memory `View`, which is the same state
  `set_model_position` writes. The C++ routes a model drag through
  `BeginLocalScreenEdit`, so it also switches screen/output when the pointer
  leaves the window and persists the result; neither exists here, so the move
  lasts until the process exits and the input region follows the drawn pixels.
- There is no **screen-edit mode**, so `hidden && !screenEditing` is simply
  `hidden`, and the C++ widening of the input region to the whole surface during
  an edit never applies.
- Motion priority: the idle motion has its own queue and a tap motion has a
  second one, so a `TapBody` motion is never refused by the idle priority rules.
  The C++ shares one manager and uses `PriorityNormal` over `PriorityIdle`; the
  visible effect (a tap motion plays over the idle) is the same.

### Status and diagnostics

`get_status` keeps its headless reply exactly as before. In overlay mode it adds:

| field | meaning |
| --- | --- |
| `renderer_available` | `true` only after a renderer was created **and** a frame was drawn |
| `frames_drawn` | frames presented so far |
| `visible_pixels` | pixels with alpha above the input-region threshold |
| `transparent_pixels` | fully transparent pixels |
| `pixel_checksum` | checksum of the last readback, for change detection |
| `width`, `height` | surface size |
| `input_regions` | rectangles in the applied input region (`0` = fully click-through) |
| `input_region_pixels` | total area those rectangles cover |
| `input_region_applied` | whether `set_input_region` accepted the region for that frame |
| `renderer_error` | why rendering failed, or `null` |
| `pointer_events` | pointer events drained from the overlay so far |
| `taps` | clicks dispatched to hit testing |
| `double_clicks` | second clicks that cancelled a pending tap |
| `drags` | presses released beyond the tap threshold |
| `right_clicks` | right releases (each advances the model) |
| `middle_clicks` | middle presses (no skins, so no effect) |
| `scrolls` | wheel events applied to the zoom |
| `ignored_hidden` | button/scroll events dropped by the hidden-model guard |
| `last_hit` | `"Head"`, `"Body"` or `null` for the last resolved tap |
| `expressions_started` | expressions a tap really started |
| `motions_started` | `TapBody` motions a tap really started |
| `hit_areas_declared` | whether the model declares `HitAreas` |
| `hit_test_available` | whether the declared hit areas could be tested |
| `interaction_error` | why the last tap started nothing, or `null` |

`debug_frame` forces one frame plus a fresh readback and returns the same
counts, plus the exact `input_region` rectangle list. Passing
`{"args":{"id":"ParamAngleX"}}` also reports the **rendered** model's current
value for that parameter. It is the test seam for the live compositor test.

## OCR commands

OCR is optional, and its two model directories come from configuration, not from
the command line.

```sh
deskwaifu-rust --model /path/to/Haru.model3.json --socket /tmp/model.sock
```

`get_ocr_status` then reports the resolved `detection_model_dir` and
`recognition_model_dir`. They are resolved in this order:

1. `--ocr-detection-dir` / `--ocr-recognition-dir`, which must be given together
   and must each be an absolute path;
2. otherwise `<configRoot>/plugin/perception/config.json` — the file the C++
   `OcrProcessor` reads — with the keys `ocr_detection_model_dir` and
   `ocr_recognition_model_dir`. `configRoot` is the directory holding the main
   `deskwaifu/config.json`, so the full path is
   `$XDG_CONFIG_HOME/deskwaifu/plugin/perception/config.json`, falling back to
   `$HOME/.config/deskwaifu/plugin/perception/config.json` when
   `XDG_CONFIG_HOME` is unset or empty;
3. a key that is absent, or a file that does not exist, keeps the documented
   system default for that directory:
   `/usr/share/deskwaifu/ocr/models/PP-OCRv6_small_det` and
   `.../PP-OCRv6_small_rec`.

A present value that is empty, not a JSON string, contains a NUL, is not
absolute, or uses `~` or an environment variable is a **startup error** naming
the key and the problem. `~` and environment variables are never expanded and
nothing silently falls back to a system path. A configuration file that cannot
be read, is not UTF-8, is not a JSON object or contains a duplicate key is also a
startup error naming the file: this runtime fails loudly instead of the C++
`OcrClient::ConfigureModels(config, configError)` path, which only marked OCR
`invalid_config` and left the application running.

A model directory that is simply not installed is **not** a startup error: the
process starts and `get_ocr_status` answers `ok:true` with
`availability_state:"unavailable"`, `error_code:"model_missing"` and an error
naming the missing path; `ocr_recognize` fails with that text. A failed model
load is not retried in the background either — a broken deployment stays
reported rather than being polled.

The four commands mirror `plugins/perception/private/processor/ocr`:

| command | reply |
| --- | --- |
| `get_ocr_status` | `ok`, `available`, `availability_state` (`unchecked`/`available`/`unavailable`), `error_code`, `error`, both model dirs, `state`, `job_id`, `loaded`, `running`, `busy`, `retiring`, `execution_mode` |
| `ocr_recognize` | `{ok:true, job_id, state:"pending"}` at once, or `ok:false` with the C++ text |
| `get_ocr_result` | `{ok:true, job_id, state}`; once terminal also `cancelled`, `response` (native `{ok,result,protocol,id}`) and, for a completion, `text` |
| `cancel_ocr` | `{ok:true, job_id}` for the matching **pending** job, otherwise `ok:false` |

`image` must be a string naming an absolute, readable regular file; every other
case (missing field, non-string, relative, directory, unreadable) fails with
`image must be an absolute readable file path`. A second `ocr_recognize` while a
job is in flight fails with `OCR is busy` — requests are never queued. Only the
most recent job is retained: accepting a new one evicts the previous receipt, so
its id then reports `unknown OCR job; only the most recent job is retained`.
`text` is `OcrProcessor`'s newline join of the line texts, and `response.result`
keeps the engine's `width`/`height`/`lines`/`elapsed_s` keys with each line's
`text`, `confidence` and four-point `polygon`. `cancelled` and `text` are
additions this module needs because it serves the commands directly instead of
routing an event to `reading_clip`; the C++ client carried neither.

### Threading and cancellation

The engine is loaded lazily on the first accepted job, on the one worker thread
this module owns; the two ONNX sessions are created, used and dropped only there,
so `--model` startup and the IPC accept loop never wait on them. The loop sends
at most one request and drains the worker's reply channel on its existing 16 ms
tick (`ocr.poll()`), so a load or an inference never blocks a request. A directory
that is not on disk is detected at construction, so its job is refused before any
work starts and `get_ocr_status` carries the reason from the first tick. A load
failure that only surfaces during `Engine::load` is reported through the job's
`response.error` and stays in `get_ocr_status`, and the model is not retried.
Only such a load failure makes `get_ocr_status` report `unavailable`; a per-job
inference failure (an undecodable image, say) fails that job and leaves OCR
available, because the in-process engine is still usable. The C++ client disabled
itself there because a failing helper subprocess was all it had.

Cancellation is **host-side only**: `deskwaifu-ocr-engine` has no cancel hook and
ONNX Runtime cannot be interrupted mid-`Run`. `cancel_ocr` marks the job failed
and cancelled and sets a flag the worker checks before it starts inferring, so a
job cancelled during model load never runs. A job cancelled mid-inference keeps
running; its result is discarded on arrival, never becomes `text`, and the
`busy` flag stays set until that late result lands (which is what the C++ client
reported while its worker drained). `get_ocr_status` therefore never claims a
cancel stopped work that is still executing.

## Speech commands

Speech is always available; it only fails when it really fails. The MiniMax
credential is stored in `<configRoot>/plugin/speech/config.json`, the same
private file the C++ `SpeechSettings` owns, where `configRoot` is
`QStandardPaths::ConfigLocation` (`$XDG_CONFIG_HOME`, else `~/.config`).

| command | reply |
| --- | --- |
| `get_speech_settings` | `{ok, configured, error}`; `error` is `""` on success |
| `set_speech_settings` | `{ok, configured, error}`, accepting `api_key` as a string; `""` clears it |
| `speak` | `{ok:true}` once the request is queued, else `ok:false` with the C++ text |
| `get_speech_status` | `{ok, active, id, state, configured, last_error}` |

`set_speech_settings` never returns the key, never echoes the request, and
answers every rejection (missing or non-string `api_key`, a duplicated key, an
over-long value, an ASCII control character, a storage failure) with the single
sentence `Unable to read or update speech settings`, leaving the stored key
untouched. A rejected write keeps `configured` describing the last key that was
applied; `get_speech_settings` re-reads the file first.

`speak` is the JSON-command form of the C++ `speech.speak/v1` capability
(`{text, replace}`):

- blank or whitespace text fails with `speech text is empty` and does not
  disturb a playback in progress;
- text of 10000 or more UTF-16 code units fails with `无效的语音文本或参数`
  (`QString::size()` counts UTF-16 units, so 5000 astral characters also fail);
- a second request while one is active fails with `speech service is busy`
  unless `replace` is `true`, which cancels the current playback and starts the
  newest;
- with no key configured it fails with
  `请先在控制面板中设置 MiniMax API Key` before any connection is opened.

`get_speech_status` is an addition of this runtime: the C++ reported progress only
through internal events. `state` is `idle`/`preparing`/`playing`/`draining`,
`id` is the most recently accepted speech id (`0` before the first one), `active`
says whether that speech is still in flight, and `last_error` is the last failure
text or `null`. `state` reaches `idle` with `active:false` only after the audio
output reports drained (SP-04), so a client that sees `draining` knows synthesis
finished but the device has not.

### Threading

One worker thread owns the tokio runtime, the protocol client and the audio
player. The IPC loop hands it at most one request through a single-slot mailbox
(a replacing request overwrites an unpicked one, so no queue can grow) and drains
its reports on the existing 16 ms tick. Nothing on the accept path blocks on the
network or the device.

### Cloud boundary

With a key configured, `speak` opens a real `wss://api.minimax.cn/ws/v1/t2a_v2_bidi`
connection and sends the text to MiniMax; without network or a valid key the
failure is reported, never faked. `--speech-endpoint <url>` overrides the address
and is a test/diagnostic seam only, since the C++ fixes the production endpoint.
`DESKWAIFU_SPEECH_TEST_SINK=1` swaps the audio device for a silent in-process sink
and `DESKWAIFU_SPEECH_TEST_SINK_DELAY_MS` slows it down.

**Untested in CI:** the suite drives the loopback endpoint and the silent sink, so
a real MiniMax synthesis and a real audio device are not covered by any automated
test.

## Clipboard reading (剪贴板朗读)

`reading_clip` runs the first-release clipboard reading-aloud pipeline: one
clipboard image snapshot → OCR → speech. It ports
`plugins/reading_clip/private/processor/reading_clip/ReadingClipProcessor.cpp`;
the state machine, the state names and the error texts are that processor's.

| command | reply |
| --- | --- |
| `reading_clip` | `{ok:true,queued:true}` at once; it only means the trigger was queued |
| `get_reading_clip_status` | `{ok,state,error}` |

`state` is one of the C++ names:

```
idle → reading_image → recognizing → speaking → idle
```

`cancelling` is externally visible too: the C++ `Status()` includes it in its
state array, so a trigger while the pipeline is busy answers `queued` and the run
enters `cancelling` before it returns to `idle` with `reading cancelled`.
Repeated triggers while `cancelling` are accepted and start no new run.

`error` is `""` when the last run succeeded or none has run. Every failure — an
unreadable clipboard, an empty recognition, an OCR/speech failure, a missing
capability or a step that ran past its 65 s budget — ends the run in `idle` with a
non-empty `error` and never starts the next step. The recognized lines are joined
with `\n` in the order OCR returned them. Completion requires the speech to
actually finish (its own machine reaching `idle` with `active:false`), not merely
that the speak request was accepted.

### Clipboard access

Wayland only. The clipboard image is read by running the system
`wl-paste --no-newline --type image/png` (wl-clipboard is a packaged runtime
dependency) on a worker thread with a 5 s timeout; its bytes are written to a
temporary PNG that lives until the OCR job is terminal (IM-02). Before OCR the
PNG header's dimensions are checked, so a side over 4096 is rejected without an
inference pass.

Rejections reuse the C++ texts where the meaning is the same:

| condition | error |
| --- | --- |
| `wl-paste` not installed | `wl-paste is not available; install wl-clipboard to read clipboard images` |
| no image on the clipboard, or empty output | `clipboard contains no image` |
| side over 4096, undecodable PNG, unwritable temp file | `image is too large or PNG encoding failed` |
| `wl-paste` did not answer in 5 s | `wl-paste did not answer within 5 s` |
| empty / whitespace-only recognition | `no text recognized` |
| run cancelled by a second trigger | `reading cancelled` |
| step over its 65 s budget | `reading capability: timed out` |

`DESKWAIFU_READING_TEST_IMAGE=<png path>` replaces the clipboard read with that
file's bytes. It is a **test-only** seam, like the C++ clipboard injection point,
and is documented here only so a test can run without a compositor.

### Divergences from the C++

- The C++ read a `QImage` from `QClipboard` and encoded it in the background;
  this runtime has no Qt, so `wl-paste` supplies the PNG bytes directly.
- The C++ cancelled a speech step through the `speech.speak/v1` cancel event.
  The speech surface here has no per-request cancel command, so a cancel during
  `speaking` stops the run from following that playback: the run ends with
  `reading cancelled` and the already-started audio is not interrupted.
- The C++ could switch a step to a dynamic capability endpoint at any time. Here
  the steps are the `ocr` and `speech` modules, so a capability that is missing or
  busy fails the step with that module's own text instead of the C++
  `reading capability: ...` sentence.

**Untested in CI:** the suite replaces the clipboard with
`DESKWAIFU_READING_TEST_IMAGE`, so no test reads a real Wayland clipboard, and the
speech peer is loopback, so nothing reaches MiniMax. The full pipeline test also
needs `libonnxruntime.so` (it is skipped like `tests/ocr.rs` otherwise) and
`CUBISM_TEST_MODEL`.

## Known gaps

- **Pointer interaction and transform editing**: taps, double-clicks, drags,
  right clicks, middle clicks and scrolls are applied (see **Pointer
  interaction**), but the C++ transform *editor* does not exist here: a drag
  moves the in-memory view without the screen-edit transaction, cross-output
  switch or config save, and `ModelFrameInputRegions(true, ..)` (whole-surface
  input during an edit) never applies. Declared `HitAreas` cannot be resolved
  because `cubism-render` exposes no drawable vertices; only the C++ fallback for
  models without hit areas is implemented.
- **Expressions, physics, pose, breathing, look-at**: `set_expression` and
  `do_motion` still fail explicitly instead of claiming success. A tap starts the
  declared expressions and `TapBody` motions for real; the `Idle` motion and eye
  blink are otherwise the only automatic animation, and physics, pose, breathing
  and look-at are not driven.
- **Window configuration**: size and anchors are fixed constants, not settings.
- **Textures are not shared** between model instances, so a model switch
  re-uploads every texture (the C++ `TextureResidency` lease does not exist here).
- **Speech is command-only**: there is no `interaction.double_click` greeting, no
  per-request cancel command, and the `speech.speak/v1` capability is served as
  the `speak` command rather than through the plugin capability bus. Voice,
  model, speed, volume and pitch are the C++ defaults, not settings.
- **OCR accuracy and real weights**: `tests/ocr.rs` proves the command surface
  and the inference wiring against constant-graph fixtures, not recognition
  quality. The engine crate's ignored `real_models` test covers real weights.

## Tests

```sh
export CUBISM_SDK_ROOT=/path/to/CubismSdkForNative-5-r.5
export GLEW_ROOT=/path/to/glew
export CUBISM_TEST_MODEL=$CUBISM_SDK_ROOT/Samples/Resources/Haru/Haru.model3.json
export LIBGL_ALWAYS_SOFTWARE=1
cargo test --locked
```

`tests/overlay.rs` skips the live compositor proof unless
`DESKWAIFU_RUNTIME_LIVE_TEST=1` (the same convention as `deskwaifu-overlay`); it
then spawns a private `kwin_wayland --virtual`, asserts real visible and
transparent pixels, that `set_parameter` changes the drawn frame, that a model
switch keeps rendering, and that the input region is nonempty, inside the
surface and non-overlapping for a drawn model, empty for a hidden one, and
restored when it is shown again. Its non-live test proves `--overlay` without
`WAYLAND_DISPLAY` fails with a clear error instead of hanging.

Two more live tests inject **real** pointer input through KWin's
`org_kde_kwin_fake_input` (the recipe `rust/overlay/tests/kwin.rs` proves; the
protocol descriptors are declared by hand here because this test target has no
`wayland-scanner` dev-dependency) and assert the runtime's own counters:

- `overlay_pointer_policy_reacts_to_real_clicks`: a click on an opaque pixel
  becomes a `tap`, a click in a transparent corner taps nothing, a right click
  advances the model (`get_status` reports `Beta`), and a hidden model stays at
  the same tap count while its region is empty.
- `overlay_tap_dispatches_real_expressions_on_a_model_without_hit_areas`: the
  same Haru assets with the `HitAreas` section removed, so the documented C++
  fallback applies; the upper half resolves `Head` and starts an expression, the
  lower half resolves `Body` and starts a `TapBody` motion plus an expression.

`src/render.rs` also has pure tests for the policy itself: the strict 10 px tap
threshold, the 350 ms/10 px double-click window, drag detection, the drag math,
`surface_to_clip`, the exact inverse of the draw projection
(`surface_to_model`), and the fallback Head/Body split.

The alpha → rectangle mapping itself has pure unit tests in `src/render.rs`
(no compositor needed): fully transparent, fully opaque, a single opaque block,
the threshold alpha values 10 and 11, bottom-up row order, and run splitting
without overlap.

`tests/ocr.rs` covers the OCR command surface. Always: that an absent
`<configRoot>/plugin/perception/config.json` reports the two system defaults,
that a key that is absent keeps only its own default, that explicit
`--ocr-*-dir` flags override a (here deliberately invalid) configuration file,
that an empty / non-string / NUL-carrying / relative value fails startup naming
its key while a merely absent directory only makes OCR unavailable, that a lone
`--ocr-detection-dir` is a startup error, and that every unusable `image` value is
rejected without creating a job. When `libonnxruntime.so` is present (real CI has
it, since the engine crate's suite runs there) it also builds a constant-graph
detector/recognizer pair, runs a real PNG end to end from both the flags and the
configuration file, and asserts the retained job's text and native result, the
busy rejection, the cancel-then-late-result discard, unknown ids, receipt eviction
and the load-failure path. Set `DESKWAIFU_SKIP_OCR_E2E=1` to skip that part
explicitly.

`tests/speech.rs` runs against a loopback MiniMax-protocol WebSocket server. It
proves the settings replies are `{ok,configured,error}` in the C++ shape, that the
key is never echoed while the file is `0600`, that empty clears and every
malformed request leaves the stored key in place, that blank and over-long text
are rejected without disturbing an active playback, that `replace` cancels and
starts the newest while a busy request without it fails, that a service rejection
surfaces as `语音服务拒绝请求（代码 1004）` and leaves the machine idle, and that
`draining` is observable before `idle` — completion waits for the audio to drain.

`tests/reading.rs` covers the clipboard reading pipeline. Always: the status
starts `idle` with an empty error, `reading_clip` answers `queued`, an unconfigured
OCR fails naming OCR without reaching speech, and a `PATH` without `wl-paste`
reports a clear error instead of hanging. With `libonnxruntime.so` it also runs
the full pipeline over the constant-graph fixtures and a loopback speech peer: the
run passes through `reading_image`, `recognizing`, `speaking` and back to `idle`
with no error, the text sent to the peer is the recognized line, the temporary
PNG is gone afterwards, a blank detection fails with `no text recognized` and
never connects, an image over 4096 per side is rejected before OCR, an
unconfigured speech key ends the run at `speaking`, and cancellation during
`recognizing` or `speaking` (including repeated triggers while `cancelling` and a
late completion) cannot resume the cancelled run.
