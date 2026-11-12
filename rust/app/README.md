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
- plays the model's **first declared motion in a group named `Idle`**, if the
  model declares one, and blinks using the `Groups` entry named `EyeBlink`.
  No motion is invented when none is declared, and there is no random selection.

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

`debug_frame` forces one frame plus a fresh readback and returns the same
counts, plus the exact `input_region` rectangle list. Passing
`{"args":{"id":"ParamAngleX"}}` also reports the **rendered** model's current
value for that parameter. It is the test seam for the live compositor test.

## OCR commands

OCR is optional. Both model directories must be supplied together:

```sh
deskwaifu-rust --model /path/to/Haru.model3.json --socket /tmp/model.sock \
  --ocr-detection-dir /usr/share/deskwaifu/ocr/models/PP-OCRv6_small_det \
  --ocr-recognition-dir /usr/share/deskwaifu/ocr/models/PP-OCRv6_small_rec
```

Without them `get_ocr_status` answers `ok:true` with
`availability_state:"unavailable"`, `error_code:"not_configured"` and an error
text naming the flags; `ocr_recognize` fails with that text. Supplying only one
directory is a startup error rather than a silent fallback to a system default.

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
tick (`ocr.poll()`), so a load or an inference never blocks a request. A failed
load is reported through the job's `response.error` and stays in
`get_ocr_status`, and the model is not retried. Only a load failure makes
`get_ocr_status` report `unavailable`; a per-job inference failure (an
undecodable image, say) fails that job and leaves OCR available, because the
in-process engine is still usable. The C++ client disabled itself there because
a failing helper subprocess was all it had.

Cancellation is **host-side only**: `deskwaifu-ocr-engine` has no cancel hook and
ONNX Runtime cannot be interrupted mid-`Run`. `cancel_ocr` marks the job failed
and cancelled and sets a flag the worker checks before it starts inferring, so a
job cancelled during model load never runs. A job cancelled mid-inference keeps
running; its result is discarded on arrival, never becomes `text`, and the
`busy` flag stays set until that late result lands (which is what the C++ client
reported while its worker drained). `get_ocr_status` therefore never claims a
cancel stopped work that is still executing.

## Known gaps

- **Pointer interaction and transform editing**: the input region decides which
  pixels are clickable, but no pointer events are consumed yet: there is no
  hit testing, dragging or tap motion. The C++ also widens the region to the
  whole surface while a transform edit is in progress
  (`ModelFrameInputRegions(true, ..)`); this runtime has no transform editor, so
  that mode does not exist rather than being faked.
- **Expressions, physics, pose, breathing, look-at**: `set_expression` and
  `do_motion` fail explicitly instead of claiming success. Only the `Idle` motion
  and eye blink are driven.
- **Window configuration**: size and anchors are fixed constants, not settings.
- **Textures are not shared** between model instances, so a model switch
  re-uploads every texture (the C++ `TextureResidency` lease does not exist here).
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

The alpha → rectangle mapping itself has pure unit tests in `src/render.rs`
(no compositor needed): fully transparent, fully opaque, a single opaque block,
the threshold alpha values 10 and 11, bottom-up row order, and run splitting
without overlap.

`tests/ocr.rs` covers the OCR command surface. Always: that no directories means
`unavailable` plus a clear `ocr_recognize` failure, that a lone
`--ocr-detection-dir` is a startup error, and that every unusable `image` value is
rejected without creating a job. When `libonnxruntime.so` is present (real CI has
it, since the engine crate's suite runs there) it also builds a constant-graph
detector/recognizer pair, runs a real PNG end to end, and asserts the retained
job's text and native result, the busy rejection, the cancel-then-late-result
discard, unknown ids, receipt eviction and the load-failure path. Set
`DESKWAIFU_SKIP_OCR_E2E=1` to skip that part explicitly.
